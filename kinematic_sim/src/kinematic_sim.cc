#include <unistd.h>
#include <iostream>
#include <string>
#include <map>
#include <math.h>

#include "ros/ros.h"

#include <boost/thread.hpp>

#include "nav_msgs/Odometry.h"
#include "geometry_msgs/Twist.h"
#include "geometry_msgs/PoseWithCovarianceStamped.h"
#include "tf/transform_broadcaster.h"
#include "tf/transform_datatypes.h"

using namespace std;

////////////////////////////////////////////////////////////////////////////////
// The class for the driver
class KinematicSimAgent
{
private:
  // The time of the last speed update
  double last_t;

  ////////////////
  // Parameters
  ////////////////

  double freq_, publish_freq_;

  string base_frame_id;
  string odom_frame_id;

  ros::NodeHandle *node_;
  ros::Publisher odom_pub;
  ros::Publisher amcl_pose_pub;
  ros::Subscriber cmd_vel_sub;
  ros::Subscriber initialpose_sub;

  tf::TransformBroadcaster broadcaster;


  boost::recursive_mutex state_lock_;
  boost::recursive_mutex vw_lock_;
  string name;

public:
  double x, y, th, v, w;
  nav_msgs::Odometry state;
  geometry_msgs::PoseWithCovarianceStamped amcl_pose;

  // Constructor; need that
  KinematicSimAgent(ros::NodeHandle *node, const string &name="", 
		    double _x=0.0, double _y=0.0, double _th=0.0)
  {
    this->node_ = node;
    this->name = name;
    odom_pub = node_->advertise<nav_msgs::Odometry>("/"+name+"/odom", 100);
    amcl_pose_pub = 
      node_->advertise<geometry_msgs::PoseWithCovarianceStamped>("/"+name+"/gt_pose", 100);
    cmd_vel_sub = node_->subscribe("/"+name+"/cmd_vel", 1, 
				   &KinematicSimAgent::OnVelCmd, this);
	  initialpose_sub = node_->subscribe("/"+name+"/initialpose", 1, 
				   &KinematicSimAgent::OnInitialPose, this);

    node_->param(string("base_frame_id"), base_frame_id, string("/base_link"));
    node_->param(string("odom_frame_id"), odom_frame_id, string("/odom"));

    // ensure that frame id begins with / character
    if (base_frame_id.compare(0, 1, "/", 1) != 0)
      base_frame_id = string("/") + base_frame_id;
    if (odom_frame_id.compare(0, 1, "/", 1) != 0)
      odom_frame_id = string("/") + odom_frame_id;

    base_frame_id = name + base_frame_id;
    odom_frame_id = name + odom_frame_id;

    state.header.frame_id = odom_frame_id;
    state.child_frame_id = base_frame_id;

    node_->param("freq", freq_, 50.0);
    node_->param("publish_freq", publish_freq_, 10.0);
    
    // odometry starts at zero
    state.pose.pose.position.x = 0.0;
    state.pose.pose.position.y = 0.0;
    state.pose.pose.position.z = 0.0;
    state.pose.pose.orientation = tf::createQuaternionMsgFromYaw(0.0);

    state.twist.twist.linear.x = 0.0;
    state.twist.twist.linear.y = 0.0;
    state.twist.twist.linear.z = 0.0;
    state.twist.twist.angular.x = 0.0;
    state.twist.twist.angular.y = 0.0;
    state.twist.twist.angular.z = 0.0;

    this->x = _x;
    this->y = _y;
    this->th = _th;

    this->last_t=ros::WallTime::now().toSec();

    this->v = this->w = 0.0;
  }

  ~KinematicSimAgent()
  {

  }

  bool SpinPublish()
  {
    ros::Rate rpub(this->publish_freq_);
    while(node_->ok())
      {
        PublishPosition();
	rpub.sleep();
      }

    return true;
  }

  bool SpinIntegration()
  {

    ros::Rate r(this->freq_);
    while(node_->ok())
      {
	IntegrateOdometry();
	r.sleep();
      }

    return true;
  }


  // Integrate odometry right now, using the current speed values
  // (last_v and last_w below)
  void IntegrateOdometry()
  {
    // find time difference since last integration
    double t, dt;
    t = ros::WallTime::now().toSec();
    dt = t - last_t;
    last_t = t;

    // cosine/sine taylor-expanded integrated expression
    double dx,dy,dth;
    {
      boost::recursive_mutex::scoped_lock lock(vw_lock_);
      dx = this->v*(dt-(this->w*this->w)*(dt*dt*dt)/6.0);
      dy = this->v*(this->w*dt*dt/2.0-(this->w*this->w*this->w)*(dt*dt*dt*dt)/24.0);
      dth = this->w*dt;
    }

    // now add to the current estimate
    {
      boost::recursive_mutex::scoped_lock lock(state_lock_);

      if(isnan(this->x)) {
	ROS_ERROR("[Integrate0] X is nan?!?");
	ROS_ERROR_STREAM("[Integrate0] [" << this->name << "] " << v << ", " << w);
	this->x = -1.0;
      }

      this->x += dx*cos(this->th) - dy*sin(this->th);
      this->y += dx*sin(this->th) + dy*cos(this->th);
      this->th += dth;

      if(isnan(this->x)) {
	ROS_ERROR("[Integrate] X is nan?!?");
	ROS_ERROR_STREAM("[Integrate] [" << this->name << "] " << dx << ", " << dy << ", " << dth);
	this->x = -1.0;
      }
    }



  }

  void PublishPosition()
  {
    boost::recursive_mutex::scoped_lock lock(state_lock_);

    //ROS_INFO_STREAM("[" << this->name << "] " << x << ", " << y << ", " << th);

    state.pose.pose.position.x = this->x; 
    state.pose.pose.position.y = this->y;
    state.pose.pose.orientation = tf::createQuaternionMsgFromYaw(this->th);
    {
      boost::recursive_mutex::scoped_lock vwlock(vw_lock_);
      state.twist.twist.linear.x = this->v;
      state.twist.twist.angular.z = this->w;
    }

    state.header.stamp = ros::Time::now();

    odom_pub.publish(state);

    if(isnan(this->x)) {
      ROS_ERROR("X is nan?!?");
      ROS_ERROR_STREAM("[" << this->name << "] " << v << ", " << w);
      this->x = -1.0;
    }
    if(isnan(this->y)) {
      ROS_ERROR("Y is nan?!?");
      ROS_ERROR_STREAM("[" << this->name << "] " << v << ", " << w);
      this->y = -1.0;
    }
    if(isnan(this->th)) {
      ROS_ERROR("Theta is nan?!?");
      ROS_ERROR_STREAM("[" << this->name << "] " << v << ", " << w);
      this->th = -1.0;
    }

    tf::Transform transform;
    transform.setOrigin(tf::Vector3(this->x, this->y, 0));
    transform.setRotation(tf::createQuaternionFromYaw(this->th));
    broadcaster.sendTransform(tf::StampedTransform(transform, ros::Time(state.header.stamp), 
                                                   odom_frame_id, base_frame_id ));

    amcl_pose.header.stamp = ros::Time::now();
    amcl_pose.header.frame_id = odom_frame_id;

    amcl_pose.pose.pose.position.x = this->x;
    amcl_pose.pose.pose.position.y = this->y;
    amcl_pose.pose.pose.orientation = tf::createQuaternionMsgFromYaw(this->th);
    amcl_pose_pub.publish(amcl_pose);
  }

  void OnVelCmd(const geometry_msgs::TwistConstPtr &input) 
  {
    ROS_DEBUG("Recieved velocity command: %f %f", input->linear.x, input->angular.z);

    boost::recursive_mutex::scoped_lock lock(vw_lock_);

    this->v = input->linear.x;
    this->w = input->angular.z;
  }

  void OnInitialPose(const geometry_msgs::PoseWithCovarianceStampedConstPtr &input) 
  {
    ROS_DEBUG_STREAM("Recieved inital pose: " << input->pose.pose);

    tf::Quaternion q;
    tf::quaternionMsgToTF(input->pose.pose.orientation, q);

    this->x = input->pose.pose.position.x;
    this->y = input->pose.pose.position.y;
    this->th = tf::getYaw(q);
  }
};

void AgentIntegrate(KinematicSimAgent *agent)
{
  agent->SpinIntegration();
}

void AgentPublish(KinematicSimAgent *agent)
{
  agent->SpinPublish();
}

class KinematicSim
{
private:
  map<string, KinematicSimAgent*> agents;
  map<string, boost::thread*> integrate_threads;
  map<string, boost::thread*> publish_threads;

public:
  ~KinematicSim()
  { 
    for(map<string, KinematicSimAgent*>::iterator i=agents.begin();
	i != agents.end();
	++i)
      {
	delete integrate_threads[i->first];
	delete publish_threads[i->first];
	delete i->second;
      }
  }

  KinematicSim(ros::NodeHandle *n_)
  {
    int num_agents;
    n_->param("num_agents", num_agents, 0);
    for(int i=0; i < num_agents; ++i) {
      stringstream name_key, initial_pos_key;;
      string name;
      string initial_pos;
      name_key << "agent" << i;
      initial_pos_key << "initial" << i;

      n_->param(name_key.str(), name, name_key.str());
      n_->param(initial_pos_key.str(), initial_pos, string("0.0 0.0 0.0"));

      double x, y, th;
      stringstream initial_pos_str(initial_pos);
      initial_pos_str >> x >> y >> th;

      ROS_INFO_STREAM("Adding agent: [" << name << "] @ " << x << ", " << y << ", " << th << " (" << initial_pos << ")");

      agents[name] = new KinematicSimAgent(n_, name, x, y, th);
    }
  }

  void start()
  {
    for(map<string, KinematicSimAgent*>::iterator i=agents.begin();
	i != agents.end();
	++i)
      {
	integrate_threads.insert(make_pair(i->first, new boost::thread(AgentIntegrate, i->second)));
	publish_threads.insert(make_pair(i->first, new boost::thread(AgentPublish, i->second)));
      }
  }

  void stop()
  {
    for(map<string, KinematicSimAgent*>::iterator i=agents.begin();
	i != agents.end();
	++i)
      {
	integrate_threads[i->first]->join();
	publish_threads[i->first]->join();
      }
  }
};

int main(int argc, char **argv)
{
  ros::init(argc, argv, "KinematicSim");
  ros::NodeHandle n("~");
  KinematicSim d(&n);

  d.start();

  ros::spin();

  d.stop();

  return(0);
}

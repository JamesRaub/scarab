#include <unistd.h>
#include <string.h>
#include <math.h>

#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>

#include "ros/ros.h"

#include "nav_msgs/Odometry.h"
#include "geometry_msgs/Twist.h"
#include "tf/transform_broadcaster.h"
#include "dynamic_reconfigure/server.h"

#include "roboclaw/RoboclawConfig.h"
#include "roboclaw/motor_state.h"
#include "RoboClaw.h"

using namespace std;

class DifferentialDriver {
public:
  explicit DifferentialDriver(const ros::NodeHandle &node) :
    nh_(node), claw_(NULL), ser_(NULL), serial_errs_(0)  {
    nh_.param("axle_width", axle_width_, 0.255);
    nh_.param("max_wheel_vel", max_wheel_vel_, 0.8);
    nh_.param("min_wheel_vel", min_wheel_vel_, 0.00);
    nh_.param("accel_max", accel_max_, 1.0);
    nh_.param("wheel_diam", wheel_diam_, 0.1);
    nh_.param("quad_pulse_per_motor_rev", quad_pulse_per_motor_rev_, 2000.0);
    nh_.param("motor_to_wheel_ratio", motor_to_wheel_ratio_, 40.0);
    nh_.param("pid_param_p", pid_p_, 15000);
    nh_.param("pid_param_i", pid_i_, 0x0250);
    nh_.param("pid_param_d", pid_d_, 500);
    nh_.param("pid_qpps", pid_qpps_, 300000);
    nh_.param("left_sign", left_sign_, -1);
    nh_.param("right_sign", right_sign_, 1);
    nh_.param("portname", portname_, std::string("/dev/roboclaw"));
    nh_.param("address", address_, 0x80);
    double motor_rev_per_meter = motor_to_wheel_ratio_ / (M_PI * wheel_diam_);
    quad_pulse_per_meter_ = quad_pulse_per_motor_rev_ * motor_rev_per_meter;
    accel_max_quad_ = accel_max_ * quad_pulse_per_meter_;

    ser_.reset(new USBSerial());
    openUsb();
    claw_.reset(new RoboClaw(ser_.get()));
    setupClaw();

    pub_ = nh_.advertise<roboclaw::motor_state>("motor_state", 5);
    setVel(0.0, 0.0);
  }

  ~DifferentialDriver() {
    if (claw_ != NULL) {
      setVel(0.0, 0.0);
    }
  }

  void setupClaw() {
    ROS_INFO("Setting PID params: P=%i I=%i D=%i QPPS=%i",
             pid_p_, pid_i_, pid_d_, pid_qpps_);
    claw_->SetM1Constants(address_, pid_d_, pid_p_, pid_i_, pid_qpps_);
    claw_->SetM2Constants(address_, pid_d_, pid_p_, pid_i_, pid_qpps_);
  }

  void ReconfigureCallback(roboclaw::RoboclawConfig &config, uint32_t level) {
    if (config.pid_p != pid_p_ || config.pid_i != pid_i_ ||
        config.pid_d != pid_d_ || config.pid_qpps != pid_qpps_) {
      pid_p_ = config.pid_p;
      pid_i_ = config.pid_i;
      pid_d_ = config.pid_d;
      pid_qpps_ = config.pid_qpps;
      setupClaw();
    }

    ROS_INFO("Updating wheel & motor params");
    quad_pulse_per_motor_rev_ = config.quad_pulse_per_motor_rev;
    motor_to_wheel_ratio_ = config.motor_to_wheel_ratio;
    wheel_diam_ = config.wheel_diam;
    accel_max_ = config.accel_max;
    min_wheel_vel_ = config.min_wheel_vel;
    max_wheel_vel_ = config.max_wheel_vel;
    axle_width_ = config.axle_width;

    double motor_rev_per_meter = motor_to_wheel_ratio_ / (M_PI * wheel_diam_);
    quad_pulse_per_meter_ = quad_pulse_per_motor_rev_ * motor_rev_per_meter;
    accel_max_quad_ = accel_max_ * quad_pulse_per_meter_;
  }

  // Convert linear / angular velocity to left / right motor speeds in meters /
  // second
  void vwToWheelSpeed(double v, double w, double *left_mps, double *right_mps) {
    // Compute the differential drive speeds from the input
    *left_mps = v - (axle_width_ / 2.0) * w;
    *right_mps = v + (axle_width_ / 2.0) * w;

    // Scale the speeds to respect the wheel speed limit
    double limitk = 1.0;
    if (fabs(*left_mps) > max_wheel_vel_) {
      limitk = max_wheel_vel_ / fabs(*left_mps);
    }
    if (fabs(*right_mps) > max_wheel_vel_) {
      double rlimitk = max_wheel_vel_ / fabs(*right_mps);
      if (rlimitk < limitk) {
        limitk = rlimitk;
      }
    }

    if (limitk != 1.0) {
      *left_mps *= limitk;
      *right_mps *= limitk;
    }

    // Deal with min limits
    if (fabs(*left_mps) < min_wheel_vel_) {
      *left_mps = 0.0;
    } if (fabs(*right_mps) < min_wheel_vel_) {
      *right_mps = 0.0;
    }

    *right_mps *= right_sign_;
    *left_mps *= left_sign_;
  }

  // Command motors to a given linear and angular velocity
  void setVel(double v, double w) {
    state_.v_sp = v;
    state_.w_sp = w;

    vwToWheelSpeed(v, w, &state_.left_sp, &state_.right_sp);

    // Convert speeds to quad pulses per second
    state_.left_qpps_sp =
      static_cast<int32_t>(round(state_.left_sp * quad_pulse_per_meter_));
    state_.right_qpps_sp =
      static_cast<int32_t>(round(state_.right_sp * quad_pulse_per_meter_));

    try {
      claw_->SpeedAccelM1(address_, accel_max_quad_, state_.left_qpps_sp);
    } catch (USBSerial::Exception &e) {
      ROS_WARN("Problem with SpeecAccel on motor 1 (error=%s)", e.what());
      serialError();
      return;
    }

    try {
      claw_->SpeedAccelM2(address_, accel_max_quad_, state_.right_qpps_sp);
    } catch (USBSerial::Exception &e) {
      ROS_WARN("Problem with SpeecAccel on motor 2 (error=%s)", e.what());
      serialError();
      return;
    }

    pub_.publish(state_);
  }

  // Read actual speed of motors and update state
  void update() {
    uint8_t status;
    int32_t speed;
    bool valid;

    try {
      speed = claw_->ReadISpeedM1(address_, &status, &valid) * 125;
    } catch (USBSerial::Exception &e) {
      ROS_WARN("Problem reading motor 1 speed (error=%s)", e.what());
      serialError();
      return;
    }

    // When using USB roboclaw it seems data is rarely, if ever, invalid.
    // Instead it looks like the device sends -EPROTO=-71 and the cdc_acm
    // driver eats that and doesn't update the file descriptor we're talking to
    if (valid && (status == 0 || status == 1)) {
      state_.left_qpps = speed;
    } else {
      ROS_WARN("Invalid data from motor 1");
      serialError();
      return;
    }

    try {
      speed = claw_->ReadISpeedM2(address_, &status, &valid) * 125;
    } catch(USBSerial::Exception &e) {
      ROS_WARN("Problem reading motor 2 speed (error=%s)", e.what());
      serialError();
      return;
    }

    if (valid && (status == 0 || status == 1)) {
      state_.right_qpps = speed;
    } else {
      ROS_WARN("Invalid data from motor 2");
      serialError();
      return;
    }

    // Convert qpps to meters / second
    state_.right = right_sign_ * state_.right_qpps / quad_pulse_per_meter_;
    state_.left = left_sign_ * state_.left_qpps / quad_pulse_per_meter_;

    state_.v = (state_.right + state_.left) / 2.0;
    state_.w = (state_.right - state_.left) / axle_width_;
    // ROS_INFO_STREAM("" << state_);
    pub_.publish(state_);
  }

  void serialError() {
    serial_errs_ += 1;
    if (serial_errs_ == 5) {
      ROS_ERROR("Several errors from roboclaw, restarting");
      roboclaw_restart_usb();
      openUsb();
      setupClaw();
      serial_errs_ = 0;
    }
  }

  void openUsb() {
    ROS_INFO("Connecting to %s...", portname_.c_str());
    ros::Time start = ros::Time::now();
    double notify_every = 10.0;
    double check_every = 0.25;
    std::string last_msg;
    while (ros::ok()) {
      try {
        ser_->Open(portname_.c_str());
        ROS_INFO("Connected to %s", portname_.c_str());
        break;
      } catch (USBSerial::Exception &e) {
        last_msg = e.what();
      }
      ros::Duration(check_every).sleep();
      double dur = (ros::Time::now() - start).toSec();
      if (dur > notify_every) {
        ROS_WARN_THROTTLE(notify_every,
                          "Haven't connected to %s in %.2f seconds."
                          "  Last error=\n%s",
                          portname_.c_str(), dur, last_msg.c_str());
      }
    }
  }

  // Get state as reflected by last calls to setVel() and update()
  const roboclaw::motor_state& getState() const {
    return state_;
  }

private:
  ros::NodeHandle nh_;
  ros::Publisher pub_;

  boost::scoped_ptr<RoboClaw> claw_;
  boost::scoped_ptr<USBSerial> ser_;
  string portname_;
  int address_;
  int serial_errs_;

  double axle_width_;
  double wheel_diam_;
  double motor_to_wheel_ratio_;
  // Max / min velocity of wheels in meters / second
  double min_wheel_vel_, max_wheel_vel_;
  // Maximum wheel acceleration in meters / second^2
  double accel_max_;
  // Quad pulse per second when motor is at 100%
  int pid_qpps_;
  int pid_p_, pid_i_, pid_d_;
  // +1 if positive means forward, -1 if positive means backwards
  int left_sign_, right_sign_;
  // Static values computed based on params
  double quad_pulse_per_motor_rev_;
  double quad_pulse_per_meter_;
  // Max accel in quad pulses per second per second
  uint32_t accel_max_quad_;
  // Current state of motors
  roboclaw::motor_state state_;
};


class RoboClawNode {
public:
  RoboClawNode() : node_("~") {
    boost::mutex::scoped_lock lock(driver_mutex_);

    driver_.reset(new DifferentialDriver(node_));

    node_.param("odom_frame", odom_state.header.frame_id, string("odom"));
    node_.param("base_frame", odom_state.child_frame_id, string("base"));

    node_.param("freq", freq_, 30.0);

    // Odometry starts at zero
    odom_state.pose.pose.orientation = tf::createQuaternionMsgFromYaw(0.0);
    x_ = y_ = th_ = 0.0;

    odom_pub = node_.advertise<nav_msgs::Odometry>("odom", 100);

    cmd_vel_sub = node_.subscribe("cmd_vel", 1,
                                  &RoboClawNode::OnTwistCmd, this);
  }

  // Thread safe way of setting velocity
  // Doesn't need to hold state_mutex_
  void OnTwistCmd(const geometry_msgs::TwistConstPtr &input)  {
    ROS_DEBUG("Got cmd_vel: %2.2f %2.2f", input->linear.x, input->angular.z);
    {
      boost::mutex::scoped_lock lock(driver_mutex_);
      driver_->setVel(input->linear.x, input->angular.z);
    }
  }

  // Thread safe way of updating odometry estimate and publishing state
  // Assumes state_mutex_ is held
  void UpdateVelAndPublish() {
    roboclaw::motor_state state;
    {
      boost::mutex::scoped_lock lock(driver_mutex_);
      driver_->update();
      state = driver_->getState();
      odom_state.header.stamp = ros::Time::now();
    }

    IntegrateOdometry(state);

    odom_state.pose.pose.position.x = x_;
    odom_state.pose.pose.position.y = y_;
    odom_state.pose.pose.orientation = tf::createQuaternionMsgFromYaw(th_);
    odom_state.twist.twist.linear.x = state.v;
    odom_state.twist.twist.angular.z = state.w;

    odom_pub.publish(odom_state);

    tf::Transform transform;
    transform.setOrigin(tf::Vector3(x_, y_, 0));
    transform.setRotation(tf::createQuaternionFromYaw(th_));
    broadcaster.sendTransform(tf::StampedTransform(transform,
                                                   ros::Time(odom_state.header.stamp),
                                                   odom_state.header.frame_id,
                                                   odom_state.child_frame_id));
  }

  // Integrate odometry given motor's current speed
  // Assumes state_mutex_ is held
  void IntegrateOdometry(const roboclaw::motor_state &state) {
    ros::Time now = ros::Time::now();
    double dt = (now-last_vel_update).toSec();

    if(dt > 10.0) {
      last_vel_update = now;
      return;
    }

    // cosine/sine taylor-expanded integrated expression
    double dx,dy,dth;
    dx = state.v * (dt - (state.w*state.w) * (dt*dt*dt) / 6.0);
    dy = state.v * (state.w*dt*dt/2.0
                    -(state.w*state.w*state.w)*
                    (dt*dt*dt*dt)/24.0);
    dth = state.w * dt;

    // now add to the current estimate
    x_ += dx*cos(th_) - dy*sin(th_);
    y_ += dx*sin(th_) + dy*cos(th_);
    th_ += dth;

    last_vel_update = now;
  }

  void ReconfigureCallback(roboclaw::RoboclawConfig &config, uint32_t level) {
    {
      boost::mutex::scoped_lock lock(state_mutex_);
      if (odom_state.header.frame_id != config.odom_frame) {
        ROS_INFO("Setting odom_frame to %s", config.odom_frame.c_str());
        odom_state.header.frame_id = config.odom_frame;
      }

      if (odom_state.child_frame_id != config.base_frame) {
        ROS_INFO("Setting base_frame to %s", config.base_frame.c_str());
        odom_state.child_frame_id = config.base_frame;
      }
      freq_ = config.freq;
    }

    {
      boost::mutex::scoped_lock lock(driver_mutex_);
      driver_->ReconfigureCallback(config, level);
    }
  }

  void Spin() {
    double curr_freq = freq_;
    ros::Rate r(curr_freq);
    while (node_.ok()) {
      {
        boost::mutex::scoped_lock lock(state_mutex_);
        if (curr_freq != freq_) {
          ROS_INFO("Updating rate to %.3fhz", freq_);
          curr_freq = freq_;
          r = ros::Rate(curr_freq);
        }
        UpdateVelAndPublish();
      }
      r.sleep();
    }
  }

private:
  // Motor parameters
  boost::scoped_ptr<DifferentialDriver> driver_;
  boost::mutex driver_mutex_; // Guards access to motors

  boost::mutex state_mutex_; // Guards access to my state
  // How frequently to read speed and update odometry
  double freq_;

  // Current x, y, theta estimate given odometry
  ros::Time last_vel_update;
  nav_msgs::Odometry odom_state;
  double x_, y_, th_;

  ros::NodeHandle node_;
  ros::Publisher odom_pub;
  ros::Subscriber cmd_vel_sub;
  ros::Subscriber param_sub;
  tf::TransformBroadcaster broadcaster;
};

int main(int argc, char **argv) {
  ros::init(argc, argv, "motor");
  RoboClawNode rcn;

  dynamic_reconfigure::Server<roboclaw::RoboclawConfig> server;
  server.setCallback(boost::bind(&RoboClawNode::ReconfigureCallback, &rcn, _1, _2));

  boost::thread motor_thread(&RoboClawNode::Spin, &rcn);
  ros::spin();

  // Thread should shutdown once ROS is done
  motor_thread.join();

  return 0;
}
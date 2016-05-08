#include "pi_pd_control.h"
#include "math.h"
#include <Eigen/Dense>
#include "std_msgs/Bool.h"
#include "std_msgs/String.h"
#include "ardrone_velocity/filtervelocity.hpp"

PI_PD_Control::PI_PD_Control()
{
    // Publisher and Subscriber
    ros::NodeHandle params("~");
    std::string s;

    params.param<std::string>("cmd_vel_ref_topic", s, "cmd_vel_ref");
    cmd_sub = nh_.subscribe(s,1, &PI_PD_Control::InputCallback, this);
    params.param<std::string>("odometry_topic", s, "odometry/prediction");
    odo_sub = nh_.subscribe(s,1, &PI_PD_Control::OdoCallback, this, ros::TransportHints().tcpNoDelay());
    ping_sub = nh_.subscribe("ardrone/ping",1, &PI_PD_Control::PingCallback, this);

    params.param<std::string>("cmd_vel_out_topic", s, "/cmd_vel");
    cmd_pub = nh_.advertise<geometry_msgs::Twist>(s, 1);
    params.param<std::string>("cmd_vel_out_topic_stamped", s, "/cmd_vel_stamped");
    cmd_stamped_pub = nh_.advertise<geometry_msgs::Twist>(s, 1);
    ref_vel_pub = nh_.advertise<geometry_msgs::Twist>("ref_vel", 1);

    // Dynamic parameter reconfigure
    dynamic_reconfigure::Server<velocity_control::dynamic_param_configConfig>::CallbackType f;
    f = boost::bind(&PI_PD_Control::dynamic_reconfigure_callback,this, _1, _2);
    m_server.setCallback(f);

    // Default values
    i_term(0) = 0.0;
    i_term(1) = 0.0;

    gain_xy(0) = 0.45;
    gain_xy(1) = 0.15;
    gain_xy(2) = 0.05;

    wind_up(0) = 0.6;
    wind_up(1) = 0.6;

    max_output(0) = 0.5;
    max_output(1) = 0.5;

    beta = 0.9;
    derv_filter = 0;
    derv_median = 0;
    derv_smith  = 1;
}

void PI_PD_Control::PingCallback(const std_msgs::StringConstPtr &ping_msg)
{
    std::string ping_string = ping_msg->data;
    double ping = std::stod (ping_string);
    navPing = ros::Duration(ping*0.001);
}

bool first_reconfig = true;
void PI_PD_Control::dynamic_reconfigure_callback(velocity_control::dynamic_param_configConfig &config, uint32_t level)
{
    if (first_reconfig)
    {
      first_reconfig = false;
      return;     // Ignore the first call to reconfigure which happens at startup
    }
  gain_xy(0) = config.Kp;
  gain_xy(1) = config.Ki;
  gain_xy(2) = config.Kd;

  wind_up(0) = config.windup;
  wind_up(1) = config.windup;

  beta = config.beta;

  max_output(0) = config.limit_x;
  max_output(1) = config.limit_y;

  derv_filter = config.derv_filter;
  derv_median = config.derv_median;
  derv_smith = config.derv_smith;

  ROS_INFO("Pid reconfigure request: Kp: %f, Ki: %f, Kd: %f", gain_xy(0), gain_xy(1), gain_xy(2));
}

void PI_PD_Control::InputCallback(const geometry_msgs::Twist& cmd_in){
    // reference velocity
    command = cmd_in;
}

void PI_PD_Control::OdoCallback(const nav_msgs::Odometry& odo_msg){
    //measurement velocity
    odo = odo_msg;
    vel_xy(0) = odo.twist.twist.linear.x;
    vel_xy(1) = odo.twist.twist.linear.y;

    pid_control();
}

void PI_PD_Control::pid_control(){
    // Gain of PID-Control, x and y direction have the same gain
    gain_xy(0) = 0.45; //p-term
    gain_xy(1) = 0.15; //i-term
    gain_xy(2) = 0.35; //d-term

    // max speed in m/s
    Eigen::Vector2d max;
    max(0) = 0.6;
    max(1) = 0.6;

    command.linear.x = std::min(max(0), std::max(-max(0), command.linear.x));
    command.linear.y = std::min(max(1), std::max(-max(1), command.linear.y));
    command_vec(0) = command.linear.x;
    command_vec(1) = command.linear.y;

    command.angular.x = 1;
    command.angular.y = 1;

    // Time step size for derivate and integral part
    curTime = ros::Time::now();
    ros::Duration dt = curTime - oldTime;
    oldTime = curTime;

    // Error -> P-Term
    double beta = 1; // set point weighting
    error_xy(0) = beta*command.linear.x - vel_xy(0);
    error_xy(1) = beta*command.linear.y - vel_xy(1);

    Control(0,0) = error_xy(0);
    Control(0,1) = error_xy(1);

    // Derivative term (based on veloctiy change instead of error change) -> D-Term
    // Note that we put the negative part here
    Eigen::Vector2d error_vel;
    Eigen::Vector2d filtered_vel;
    filtered_vel(0) = filterx.lowpass_filter(vel_xy(0));
    filtered_vel(1) = filtery.lowpass_filter(vel_xy(1));

    error_vel(0) = -(filtered_vel(0) - old_vel_xy(0))/dt.toSec();
    error_vel(1) = -(filtered_vel(1) - old_vel_xy(1))/dt.toSec();

    Control(2,0) = error_vel(0);
    Control(2,1) = error_vel(1);

    old_vel_xy = filtered_vel;

    // Anti wind_up -> to limit Integral Part
    Eigen::Vector2d wind_up;
    wind_up(0) = 0.6;
    wind_up(1) = 0.6;

    // Intergral part with max values and reset if new reference value is received -> I-Term
    i_term_set(i_term(0), error_xy(0), command_vec(0), old_command_vec(0), wind_up(0), dt.toSec());
    i_term_set(i_term(1), error_xy(1), command_vec(1), old_command_vec(1), wind_up(1), dt.toSec());

    Control(1,0) = i_term(0);
    Control(1,1) = i_term(1);


    if (command_vec(0) != old_command_vec(0))
    {
        old_ref(0) = old_command_vec(0);
        switch_ref = true;
    }
    if (command_vec(1) != old_command_vec(1))
    {
        old_ref(1) = old_command_vec(1);
        switch_ref = true;
    }

    old_command_vec = command_vec;


    // Calculate outputs -> tilt angle output_value*12 = ref_tilt_angle
    geometry_msgs::Twist control_output_pid;
    control_output_pid.linear.x = Control.col(0).transpose()*gain_xy;
    control_output_pid.linear.y = Control.col(1).transpose()*gain_xy;

    control_output_pid.angular.x = 1;
    control_output_pid.angular.y = 1;

    //Limit control command
    max(0) = 0.5;
    max(1) = 0.5;

    control_output_pid.linear.x = std::min(max(0), std::max(-max(0), control_output_pid.linear.x));
    control_output_pid.linear.y = std::min(max(1), std::max(-max(1), control_output_pid.linear.y));
    control_output_old = control_output_pid;

    // set point control -> open loop -> set reference tilt angle based on desired velocity
    geometry_msgs::Twist control_output_feedforward;
    control_output_feedforward.linear.x = 0.37/9.81*180/3.14/12*command.linear.x;
    control_output_feedforward.linear.y = 0.37/9.81*180/3.14/12*command.linear.y;

    if(std::abs((command.linear.x - vel_xy(0))/(command.linear.x - old_ref(0))) < 0.25 || switch_ref == false)
    {
     control_output_feedforward.linear.x = 0;
     switch_ref == false;
    }
    if(std::abs((command.linear.y - vel_xy(1))/(command.linear.y - old_ref(1))) < 0.25  || switch_ref == false)
    {
     control_output_feedforward.linear.y = 0;
     switch_ref == false;
    }

    // Put the single parts together
    geometry_msgs::Twist control_output;
    control_output.linear.x = std::min(max(0), std::max(-max(0), 0.5*control_output_feedforward.linear.x + control_output_pid.linear.x));
    control_output.linear.y = std::min(max(1), std::max(-max(1), 0.5*control_output_feedforward.linear.y + control_output_pid.linear.y));

    //Debugging information
    Eigen::VectorXd tmp = Control.col(0).cwiseProduct(gain_xy);
    ROS_INFO("d_Time  : %f", dt.toSec());
    ROS_INFO("VelRef: %f , %f", command.linear.x, command.linear.y);
    ROS_INFO("Vel   : %f , %f", odo.twist.twist.linear.x, odo.twist.twist.linear.y);
    ROS_INFO("Error : %f ,  %f", error_xy(0),error_xy(1));
    ROS_INFO("Cmd   : %f , %f", control_output.linear.x,  control_output.linear.y);
    ROS_INFO("PID: %f, Feedforward: %f ",control_output_pid.linear.x,control_output_feedforward.linear.x);
    ROS_INFO("pterm | iterm | dterm   : %f | %f | %f", tmp(0), tmp(1), tmp(2));
    ROS_INFO("------------------------------------------------------");

    //We publish the command
    cmd_pub.publish(control_output);
    ref_vel_pub.publish(command);
}


void PI_PD_Control::set_hover(void)
{
    geometry_msgs::Twist control_output;
    control_output.linear.x = 0;
    control_output.linear.y = 0;
    control_output.linear.z = 0;
    control_output.angular.x = 0;
    control_output.angular.y = 0;
    control_output.angular.z = 0;

    cmd_pub.publish(control_output);
}

void PI_PD_Control::i_term_set(double &i_value, double error, double vel, double old_vel, double wind_up, double dt)
{

    if(error < 0 && i_value > 0)
    {
        i_value = std::max(0.0, i_value + error*dt);
    }

    if(error > 0 && i_value < 0)
    {
        i_value = std::min(0.0, i_value + error*dt);
    }
    else
    {
        i_value = i_value + error*dt;
    }

    if(i_value > wind_up)
    {
        i_value = wind_up;
    }

    if(i_value < - wind_up)
    {
        i_value = - wind_up;
    }

    if( vel != old_vel)
    {
        i_value = 0;
    }
}

void PI_PD_Control::run()
{
    ros::spinOnce();
}


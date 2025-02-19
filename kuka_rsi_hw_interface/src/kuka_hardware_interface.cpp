/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2014 Norwegian University of Science and Technology
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Norwegian University of Science and
 *     Technology, nor the names of its contributors may be used to
 *     endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/*
 * Author: Lars Tingelstad <lars.tingelstad@ntnu.no>
 */

#include <kuka_rsi_hw_interface/kuka_hardware_interface.h>

#include <stdexcept>

namespace kuka_rsi_hw_interface
{

  KukaHardwareInterface::KukaHardwareInterface() : joint_position_(6, 0.0), joint_velocity_(6, 0.0), joint_effort_(6, 0.0), joint_position_command_(6, 0.0), joint_velocity_command_(
                                                                                                                                                                 6, 0.0),
                                                   joint_effort_command_(6, 0.0), joint_names_(6), rsi_initial_joint_positions_(6, 0.0), rsi_joint_position_corrections_(
                                                                                                                                             6, 0.0),
                                                   ipoc_(0), n_dof_(6), des_(6, 0.0), des_prev_(6, 0.0), des_prev_prev_(6, 0.0), des_vel_(6, 0.0), des_vel_prev_(6, 0.0), des_acl_(6, 0.0)
  {
    in_buffer_.resize(1024);
    out_buffer_.resize(1024);
    remote_host_.resize(1024);
    remote_port_.resize(1024);

    if (!nh_.getParam("controller_joint_names", joint_names_))
    {
      ROS_ERROR("Cannot find required parameter 'controller_joint_names' "
                "on the parameter server.");
      throw std::runtime_error("Cannot find required parameter "
                               "'controller_joint_names' on the parameter server.");
    }

    // Create ros_control interfaces
    for (std::size_t i = 0; i < n_dof_; ++i)
    {
      // Create joint state interface for all joints
      joint_state_interface_.registerHandle(
          hardware_interface::JointStateHandle(joint_names_[i], &joint_position_[i], &joint_velocity_[i],
                                               &joint_effort_[i]));

      // Create joint position control interface
      position_joint_interface_.registerHandle(
          hardware_interface::JointHandle(joint_state_interface_.getHandle(joint_names_[i]),
                                          &joint_position_command_[i]));
    }

    // Register interfaces
    registerInterface(&joint_state_interface_);
    registerInterface(&position_joint_interface_);

    ROS_INFO_STREAM_NAMED("hardware_interface", "Loaded kuka_rsi_hardware_interface");
  }

  KukaHardwareInterface::~KukaHardwareInterface()
  {
  }

  bool KukaHardwareInterface::read(const ros::Time time, const ros::Duration period)
  {
    in_buffer_.resize(1024);

    if (server_->recv(in_buffer_) == 0)
    {
      return false;
    }

    if (rt_rsi_pub_->trylock())
    {
      rt_rsi_pub_->msg_.data = in_buffer_;
      rt_rsi_pub_->unlockAndPublish();
    }

    rsi_state_ = RSIState(in_buffer_);
    for (std::size_t i = 0; i < n_dof_; ++i)
    {
      joint_position_[i] = DEG2RAD * rsi_state_.positions[i];
      if (i < 6)
        debug_bob_.cur[i] = rsi_state_.positions[i];
    }
    ipoc_ = rsi_state_.ipoc;

    return true;
  }

  bool KukaHardwareInterface::write(const ros::Time time, const ros::Duration period)
  {
    out_buffer_.resize(1024);
    time_ = time.toSec();
    for (std::size_t i = 0; i < n_dof_; ++i)
    {
      rsi_joint_position_corrections_[i] = (RAD2DEG * joint_position_command_[i]) - rsi_initial_joint_positions_[i];
      des_[i] = (RAD2DEG * joint_position_command_[i]);
      des_vel_[i] = (des_[i] - des_prev_[i]) / (time_ - prev_time_);
      des_acl_[i] = (des_vel_[i] - des_vel_prev_[i]) / (time_ - prev_time_);

      // adding stuff for debug
      if (i < 6)
      {
        debug_bob_.des[i] = des_[i];
        debug_bob_.desf[i] = des_[i];
      }
    }
    // filter accel
    double max_accel = 300;
    double des_new = 0;
    // only slow down axis that are moving too fast, this is not ideal as position accuracy will suffer
    for (std::size_t i = 0; i < n_dof_; ++i)
    {
      if (abs(des_acl_[i]) > max_accel)
      {
        if (des_acl_[i] > 0)
          des_new = max_accel * .99 * (time_ - prev_time_) * (time_ - prev_time_) + des_vel_prev_[i] * (time_ - prev_time_) + des_prev_[i];
        else
          des_new = -max_accel * .99 * (time_ - prev_time_) * (time_ - prev_time_) + des_vel_prev_[i] * (time_ - prev_time_) + des_prev_[i];
        des_[i] = des_new;
        // need to recalculate desired velocity
        des_vel_[i] = (des_[i] - des_prev_[i]) / (time_ - prev_time_);
        debug_bob_.desf[i] = des_[i];
        // rsi_joint_position_corrections_[i] = des_new - rsi_initial_joint_positions_[i];
      }
    }
    // update values
    for (std::size_t i = 0; i < n_dof_; ++i)
    {
      des_prev_prev_[i] = des_prev_[i];
      des_prev_[i] = des_[i];
      des_vel_prev_[i] = des_vel_[i];
    }
    prev_prev_time_ = prev_time_;
    prev_time_ = time_;
    debug_bob_.time = time.toSec();
    debug_bob_.period = period.toSec();
    out_buffer_ = RSICommand(rsi_joint_position_corrections_, ipoc_).xml_doc;
    server_->send(out_buffer_);
    // only send debug info if we set up the debug server correctly earlier
    if (debug_server_working_)
    {
      debug_server_->send((char *)&debug_bob_, sizeof(debug_bob_));
    }

    return true;
  }

  void KukaHardwareInterface::start()
  {
    // Wait for connection from robot
    server_.reset(new UDPServer(local_host_, local_port_));

    // setup debug server
    // std::string d_recv_ip = "127.0.0.1";
    std::string d_recv_ip = "192.168.1.45";
    int d_recv_port = 7777;
    std::string d_send_ip = "192.168.1.44";
    // std::string d_send_ip = "127.0.0.1";
    int d_send_port = 7778;
    try
    {
      debug_server_.reset(new UDPServer(d_recv_ip, d_recv_port, d_send_ip, d_send_port));
    }
    catch (const std::runtime_error &e)
    {
      ROS_ERROR_STREAM_NAMED("kuka_hardware_interface", "Debug hardware interface server not set up! on " << d_recv_ip << " sending to " << d_send_ip << ":" << d_send_port);
      debug_server_working_ = false;
    }

    ROS_INFO_STREAM_NAMED("kuka_hardware_interface", "Waiting for robot!");

    int bytes = server_->recv(in_buffer_);

    // Drop empty <rob> frame with RSI <= 2.3
    if (bytes < 100)
    {
      bytes = server_->recv(in_buffer_);
    }

    rsi_state_ = RSIState(in_buffer_);
    for (std::size_t i = 0; i < n_dof_; ++i)
    {
      joint_position_[i] = DEG2RAD * rsi_state_.positions[i];
      joint_position_command_[i] = joint_position_[i];
      rsi_initial_joint_positions_[i] = rsi_state_.initial_positions[i];
    }
    ipoc_ = rsi_state_.ipoc;
    out_buffer_ = RSICommand(rsi_joint_position_corrections_, ipoc_).xml_doc;
    server_->send(out_buffer_);
    // Set receive timeout to 1 second
    server_->set_timeout(1000);
    ROS_INFO_STREAM_NAMED("kuka_hardware_interface", "Got connection from robot");
  }

  void KukaHardwareInterface::configure()
  {
    const std::string param_addr = "rsi/listen_address";
    const std::string param_port = "rsi/listen_port";

    if (nh_.getParam(param_addr, local_host_) && nh_.getParam(param_port, local_port_))
    {
      ROS_INFO_STREAM_NAMED("kuka_hardware_interface",
                            "Setting up RSI server on: (" << local_host_ << ", " << local_port_ << ")");
    }
    else
    {
      std::string msg = "Failed to get RSI listen address or listen port from"
                        " parameter server (looking for '" +
                        param_addr + "' and '" + param_port + "')";
      ROS_ERROR_STREAM(msg);
      throw std::runtime_error(msg);
    }
    rt_rsi_pub_.reset(new realtime_tools::RealtimePublisher<std_msgs::String>(nh_, "rsi_xml_doc", 3));
  }

} // namespace kuka_rsi_hardware_interface

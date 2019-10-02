/*
 * Copyright Notes
 *
 * Authors: Benjamin Morrell    (benjamin.morrell@jpl.nasa.gov)
 */

#ifndef LAMP_DATA_HANDLER_BASE_H
#define LAMP_DATA_HANDLER_BASE_H

// Includes 
#include <ros/ros.h>

#include <utils/CommonStructs.h>


class LampDataHandlerBase {
  public:

    LampDataHandlerBase();
    ~LampDataHandlerBase();

    bool Initialize(const ros::NodeHandle& n);

    virtual FactorData GetData() = 0;

  protected:

    // Node initialization.
    bool LoadParameters(const ros::NodeHandle& n);
    bool RegisterCallbacks(const ros::NodeHandle& n, bool from_log);
    bool RegisterLogCallbacks(const ros::NodeHandle& n);
    bool RegisterOnlineCallbacks(const ros::NodeHandle& n);
    bool CreatePublishers(const ros::NodeHandle& n);

    // Callback functions 
    // void DataCallback();

    

  private:

};


// TODO - remaptopic names for the handlers in the launch file
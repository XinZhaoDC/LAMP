/*
 * Copyright Notes
 *
 * Authors: Benjamin Morrell    (benjamin.morrell@jpl.nasa.gov)
 */


// Includes
#include <lamp/LampBase.h>

// #include <math.h>
// #include <ctime>

namespace pu = parameter_utils;
namespace gu = geometry_utils;

// Constructor
LampBase::LampBase()
  : example_variable_(3.14159),
    variable_2_(2),
    example_boolean_(false) {
     // any other things on construction 
    }

// Destructor
LampBase::~LampBase() {}

// Initialization
LampBase::Initialize() {

  LoadParameters();
  CreatePublishers();
  InitializeHandlers();

}

// Load Parameters
LampBase::LoadParameters() {

}

// Create Publishers
LampBase::CreatePublishers() {

}



// TODO might be common - check
void LampRobot::PublishPoseGraph(){
  
}


gtsam::Key LampRobot::getKeyAtTime(const ros::Time& stamp) const {

  auto iterTime = stamps_keyed_.lower_bound(stamp.toSec()); // First key that is not less than timestamp 

  // std::cout << "Got iterator at lower_bound. Input: " << stamp.toSec() << ", found " << iterTime->first << std::endl;

  // TODO - interpolate - currently just take one
  double t2 = iterTime->first;

  if (iterTime == stamps_keyed_.begin()){
    ROS_WARN("Only one value in the graph - using that");
    return iterTime->second;
  }
  double t1 = std::prev(iterTime,1)->first; 

  // std::cout << "Time 1 is: " << t1 << ", Time 2 is: " << t2 << std::endl;

  gtsam::Symbol key;

  if (t2-stamp.toSec() < stamp.toSec() - t1) {
    // t2 is closer - use that key
    // std::cout << "Selecting later time: " << t2 << std::endl;
    key = iterTime->second;
  } else {
    // t1 is closer - use that key
    // std::cout << "Selecting earlier time: " << t1 << std::endl;
    key = std::prev(iterTime,1)->second;
    iterTime--;
  }
  // std::cout << "Key is: " << key << std::endl;
  if (iterTime == std::prev(stamps_keyed_.begin())){
    ROS_WARN("Invalid time for graph (before start of graph range). Choosing next value");
    iterTime++;
    // iterTime = stamps_keyed_.begin();
    key = iterTime->second;
  } else if(iterTime == stamps_keyed_.end()) {
    ROS_WARN("Invalid time for graph (past end of graph range). take latest pose");
    key = key_ -1;
  }

  return key; 

}

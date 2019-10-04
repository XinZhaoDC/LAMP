/*
LoopClosureBase.cc
Author: Yun Chang
Loop closure base class
*/
#include "loop_closure/LoopClosureBase.h"

#include <pose_graph_msgs/PoseGraph.h>

#include <gtsam/geometry/Point3.h>
#include <gtsam/geometry/Rot3.h>

LoopClosure::LoopClosure(const ros::NodeHandle& n) {
  ros::NodeHandle nl(n);  // Nodehandle for subscription/publishing
  loop_closure_pub_ =
      nl.advertise<pose_graph_msgs::PoseGraph>("loop_closures", 10, false);
  keyed_stamps_sub_ = nl.subscribe<pose_graph_msgs::PoseGraphNode>(
      "new_node", 10, &LoopClosure::InputCallback, this);
}

LoopClosure::~LoopClosure(){};

void LoopClosure::InputCallback(
    const pose_graph_msgs::PoseGraphNode::ConstPtr& node_msg) {
  gtsam::Key new_key = node_msg->key;            // extract new key
  ros::Time timestamp = node_msg->header.stamp;  // extract new timestamp

  // also extract poses (NOTE(Yun) this pose will not be updated...)
  gtsam::Pose3 new_pose;
  gtsam::Point3 pose_translation(node_msg->pose.position.x,
                                 node_msg->pose.position.y,
                                 node_msg->pose.position.z);
  gtsam::Rot3 pose_orientation(
      gtsam::Rot3::quaternion(node_msg->pose.orientation.w,
                              node_msg->pose.orientation.x,
                              node_msg->pose.orientation.y,
                              node_msg->pose.orientation.z));
  new_pose = gtsam::Pose3(pose_orientation, pose_translation);

  // add new key and stamp to keyed_stamps_
  keyed_stamps_[new_key] = timestamp;
  // add new key and pose to keyed_poses_
  keyed_poses_[new_key] = new_pose;

  std::vector<pose_graph_msgs::PoseGraphEdge> loop_closure_edges;
  // first find loop closures
  if (FindLoopClosures(new_key, &loop_closure_edges)) {
    PublishLoopClosures(loop_closure_edges);
  }
}

void LoopClosure::PublishLoopClosures(
    const std::vector<pose_graph_msgs::PoseGraphEdge> edges) const {
  // For now publish PoseGraph messsage type (but only populate edges)
  pose_graph_msgs::PoseGraph graph;
  graph.edges = edges;

  loop_closure_pub_.publish(graph);
}

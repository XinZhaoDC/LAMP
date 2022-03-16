/*
 * Copyright Notes
 *
 * Authors: Andrzej Reinke   (andrzej.m.reinke@jpl.nasa.gov)
 */

#include <parameter_utils/ParameterUtils.h>
#include <point_cloud_mapper/SimplePointCloudMapper.h>
#include <std_msgs/String.h>

namespace pu = parameter_utils;

SimplePointCloudMapper::SimplePointCloudMapper() {
  map_data_.reset(new PointCloud);
}

SimplePointCloudMapper::~SimplePointCloudMapper() {
  if (publish_thread_.joinable()) {
    publish_thread_.join();
  }
}

bool SimplePointCloudMapper::Initialize(const ros::NodeHandle& n) {
  name_ = ros::names::append(n.getNamespace(), "SimplePointCloudMapper");

  if (!LoadParameters(n)) {
    ROS_ERROR("%s: Failed to load parameters.", name_.c_str());
    return false;
  }

  if (!RegisterCallbacks(n)) {
    ROS_ERROR("%s: Failed to register callbacks.", name_.c_str());
    return false;
  }

  return true;
}

bool SimplePointCloudMapper::LoadParameters(const ros::NodeHandle& n) {
  //  Load fixed frame.
  if (!pu::Get("frame_id/fixed", fixed_frame_id_))
    return false;
  map_data_->header.frame_id = fixed_frame_id_;

  // Load map parameters.

  if (!pu::Get("map/b_publish_only_with_subscribers",
               b_publish_only_with_subscribers_))
    return false;
  if (!pu::Get("map/b_publish_map_info", b_publish_map_info_))
    return false;
  if (!pu::Get("map/volume_voxel_size", volume_voxel_size))
    return false;

  // Clear the map buffer

  initialized_ = true;

  return true;
}

bool SimplePointCloudMapper::RegisterCallbacks(const ros::NodeHandle& n) {
  // Create a local nodehandle to manage callback subscriptions.
  ros::NodeHandle nl(n);

  map_pub_ = nl.advertise<PointCloud>("octree_map", 10, true);

  map_frozen_pub_ = nl.advertise<PointCloud>("octree_map_frozen", 10, false);
  map_info_pub_ = nl.advertise<pose_graph_msgs::MapInfo>("map_info", 10, false);

  return true;
}

void SimplePointCloudMapper::Reset() {
  map_data_->clear();
  map_data_->header.frame_id = fixed_frame_id_;
  initialized_ = true;
}

bool SimplePointCloudMapper::InsertPoints(const PointCloud::ConstPtr& points,
                                          PointCloud* incremental_points) {
  if (!initialized_) {
    ROS_ERROR("%s: Not initialized.", name_.c_str());
    return false;
  }

  //  Try to get the map mutex from the publisher. If the publisher is using it,
  //  we will just not insert this point cloud right now. It'll be added when
  // the   map is regenerated by loop closure.
  if (map_mutex_.try_lock()) {
    *map_data_ += *points;
    map_mutex_.unlock();
  } else {
    // This won't happen often.
    ROS_WARN("%s: Failed to update map: map publisher has a hold of the "
             "thread. Turn off any subscriptions to the 3D map topic to "
             "prevent this            from "
             "happening.",
             name_.c_str());
  }

  map_updated_ = true;
  return true;
}

bool SimplePointCloudMapper::ApproxNearestNeighbors(const PointCloud& points,
                                                    PointCloud* neighbors) {
  ROS_WARN_STREAM(
      name_.c_str()
      << ": This class is only implemented for visualisation, if you "
         "are using this you are doing something not correct!");
  return false;
}

void SimplePointCloudMapper::PublishMap() {
  if (map_pub_.getNumSubscribers() > 0 || !b_publish_only_with_subscribers_) {
    if (initialized_ && map_updated_) {
      // Use a new thread to publish the map to avoid blocking main thread
      // on concurrent calls.
      if (publish_thread_.joinable()) {
        publish_thread_.join();
      }
      publish_thread_ =
          std::thread(&SimplePointCloudMapper::PublishMapThread, this);
    }
  }
}

void SimplePointCloudMapper::PublishMapThread() {
  map_mutex_.lock();

  map_pub_.publish(map_data_);

  // Don't publish again until we get another map update.
  map_updated_ = false;
  map_mutex_.unlock();
}

void SimplePointCloudMapper::PublishMapFrozen() {
  if (initialized_ && map_frozen_pub_.getNumSubscribers() > 0) {
    // Use a new thread to publish the map to avoid blocking main thread
    // on concurrent calls.
    if (publish_frozen_thread_.joinable()) {
      publish_frozen_thread_.join();
    }
    publish_frozen_thread_ =
        std::thread(&SimplePointCloudMapper::PublishMapFrozenThread, this);
  }
}

void SimplePointCloudMapper::PublishMapFrozenThread() {
  map_frozen_mutex_.lock();
  map_frozen_pub_.publish(map_data_);
  // Don't publish again until we get another map update.
  map_frozen_mutex_.unlock();
}

void SimplePointCloudMapper::PublishMapUpdate(
    const PointCloud& incremental_points) {
  //  // Publish the incremental points for visualization.
  ROS_WARN_STREAM(name_.c_str()
                  << ":PublishMapUpdate: This class is only implemented for "
                     "visualisation, if you "
                     "are using this you are doing something not correct!");
}

void SimplePointCloudMapper::PublishMapInfo() {
  if (!b_publish_map_info_) {
    return;
  }

  pose_graph_msgs::MapInfo map_info;

  // If the map has been recently updated
  if (initialized_ && map_updated_) {
    // Collect map properties
    map_info.header.stamp =
        ros::Time(map_data_->header.stamp / ((uint64_t)1e6),
                  (map_data_->header.stamp % ((uint64_t)1e6)) * 1e3);
    map_info.header.frame_id = map_data_->header.frame_id;
    map_info.size = map_data_->size();
    map_info.initialized = initialized_;

    float not_used = 0.0f;

    map_info.volume = not_used;

    // Publish
    map_info_pub_.publish(map_info);
  }
}

void SimplePointCloudMapper::SetBoxFilterSize(const int box_filter_size) {
  ROS_WARN_STREAM(name_.c_str()
                  << ":SetBoxFilterSize: This class is only implemented for "
                     "visualisation, if you "
                     "are using this you are doing something not correct!");
}

void SimplePointCloudMapper::Refresh(
    const geometry_utils::Transform3& current_pose) {
  ROS_WARN_STREAM(
      name_.c_str()
      << ":Refresh: This class is only implemented for visualisation, if you "
         "are using this you are doing something not correct!");
}

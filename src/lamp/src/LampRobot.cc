/*
 * Copyright Notes
 *
 * Authors:
 * Alex Stephens       (alex.stephens@jpl.nasa.gov)
 * Benjamin Morrell    (benjamin.morrell@jpl.nasa.gov)
 * Kamak Ebadi          ()
 * Matteo               ()
 * Nobuhrio
 * Yun
 * Abhishek
 * Eric Hieden
 */

// Includes
#include <lamp/LampRobot.h>

// #include <math.h>
// #include <ctime>

namespace pu = parameter_utils;
namespace gu = geometry_utils;
namespace gr = gu::ros;

using gtsam::BetweenFactor;
using gtsam::NonlinearFactorGraph;
using gtsam::Pose3;
using gtsam::PriorFactor;
using gtsam::RangeFactor;
using gtsam::Rot3;
using gtsam::Symbol;
using gtsam::Values;
using gtsam::Vector3;

// Constructor
LampRobot::LampRobot() : 
  is_artifact_initialized(false) {
  b_run_optimization_ = false;
}

// Destructor
LampRobot::~LampRobot() {}

// Initialization - override for robot specific setup
bool LampRobot::Initialize(const ros::NodeHandle& n) {
  // Get the name of the process
  name_ = ros::names::append(n.getNamespace(), "LampRobot");

  if (!filter_.Initialize(n)) {
    ROS_ERROR("%s: Failed to initialize point cloud filter.", name_.c_str());
    return false;
  }

  if (!mapper_.Initialize(n)) {
    ROS_ERROR("%s: Failed to initialize mapper.", name_.c_str());
    return false;
  }

  // Add load params etc
  if (!LoadParameters(n)) {
    ROS_ERROR("%s: Failed to load parameters.", name_.c_str());
    return false;
  }

  // Init Handlers
  if (!InitializeHandlers(n)) {
    ROS_ERROR("%s: Failed to initialize handlers.", name_.c_str());
    return false;
  }

  // Register Callbacks
  if (!RegisterCallbacks(n)) {
    ROS_ERROR("%s: Failed to register callbacks.", name_.c_str());
    return false;
  }

  // Publishers
  if (!CreatePublishers(n)) {
    ROS_ERROR("%s: Failed to create publishers.", name_.c_str());
    return false;
  }

  return true;
}

bool LampRobot::LoadParameters(const ros::NodeHandle& n) {
  // Rates
  if (!pu::Get("rate/update_rate", update_rate_))
    return false;

  // Settings for precisions
  if (!pu::Get("b_use_fixed_covariances", b_use_fixed_covariances_))
    return false;

  // Switch on/off flag for UWB
  if (!pu::Get("b_use_uwb", b_use_uwb_))
    return false;

  // Switch on/off flag for IMU
  if (!pu::Get("b_add_imu_factors", b_add_imu_factors_))
    return false;
  if (!pu::Get("imu_factors_per_opt", imu_factors_per_opt_))
    return false;
    

  // Load frame ids.
  if (!pu::Get("frame_id/fixed", pose_graph_.fixed_frame_id))
    return false;
  if (!pu::Get("frame_id/base", base_frame_id_))
    return false;

  if (!pu::Get("b_artifacts_in_global", b_artifacts_in_global_))
    return false;

  if (!pu::Get("time_threshold", pose_graph_.time_threshold))
    return false;
  // Load filtering parameters.
  if (!pu::Get("filtering/grid_filter", params_.grid_filter))
    return false;
  if (!pu::Get("filtering/grid_res", params_.grid_res))
    return false;
  if (!pu::Get("filtering/random_filter", params_.random_filter))
    return false;
  if (!pu::Get("filtering/decimate_percentage", params_.decimate_percentage))
    return false;
  // Cap to [0.0, 1.0].
  params_.decimate_percentage =
      std::min(1.0, std::max(0.0, params_.decimate_percentage));

  // TODO - bring in other parameter

  // Set Precisions
  // TODO - eventually remove the need to use this
  if (!SetFactorPrecisions()) {
    ROS_ERROR("SetFactorPrecisions failed");
    return false;
  }

  // Set the initial key - to get the right symbol
  if (!SetInitialKey()) {
    ROS_ERROR("SetInitialKey failed");
    return false;
  }

  // Set the initial position (from fiducials) - also inits the pose-graph
  if (!SetInitialPosition()) {
    ROS_ERROR("SetInitialPosition failed");
    return false;
  }

  // Timestamp to keys initialization (initilization is particular to the robot
  // version of lamp)
  ros::Time stamp = ros::Time::now();
  pose_graph_.InsertKeyedStamp(pose_graph_.initial_key, stamp);
  pose_graph_.InsertStampedOdomKey(stamp.toSec(), pose_graph_.initial_key);

  // Set initial key
  pose_graph_.key = pose_graph_.initial_key + 1;

  return true;
}

bool LampRobot::RegisterCallbacks(const ros::NodeHandle& n) {
  // Create a local nodehandle to manage callback subscriptions.
  ros::NodeHandle nl(n);

  update_timer_ =
      nl.createTimer(update_rate_, &LampRobot::ProcessTimerCallback, this);

  back_end_pose_graph_sub_ = nl.subscribe("optimized_values",
                                          1,
                                          &LampRobot::OptimizerUpdateCallback,
                                          dynamic_cast<LampBase*>(this));

  laser_loop_closure_sub_ = nl.subscribe("laser_loop_closures",
                                         1,
                                         &LampRobot::LaserLoopClosureCallback,
                                         dynamic_cast<LampBase*>(this));

  return true;
}

bool LampRobot::CreatePublishers(const ros::NodeHandle& n) {
  // Creates pose graph publishers in base class
  LampBase::CreatePublishers(n);

  // Create a local nodehandle to manage callback subscriptions.
  ros::NodeHandle nl(n);

  // Pose Graph publishers
  pose_graph_to_optimize_pub_ = nl.advertise<pose_graph_msgs::PoseGraph>(
      "pose_graph_to_optimize", 10, false);
  keyed_scan_pub_ =
      nl.advertise<pose_graph_msgs::KeyedScan>("keyed_scans", 10, false);

  // Publishers
  pose_pub_ = nl.advertise<geometry_msgs::PoseStamped>("lamp_pose", 10, false);

  return true;
}

bool LampRobot::SetInitialKey() {
  // Get the robot prefix from launchfile to set initial key
  // TODO - get this convertor setup to Kyon
  unsigned char prefix_converter[1];

  if (!pu::Get("robot_prefix", pose_graph_.prefix)) {
    ROS_ERROR(
        "Could not find node ID assosiated with robot_namespace [LampRobot]");
    pose_graph_.initial_key = 0;
    return false;
  } else {
    std::copy(
        pose_graph_.prefix.begin(), pose_graph_.prefix.end(), prefix_converter);
    pose_graph_.initial_key = gtsam::Symbol(prefix_converter[0], 0);
    return true;
  }
}

bool LampRobot::SetInitialPosition() {
  // Load initial position and orientation.
  double init_x = 0.0, init_y = 0.0, init_z = 0.0;
  double init_qx = 0.0, init_qy = 0.0, init_qz = 0.0, init_qw = 1.0;
  // if (pose_graph_.prefix.at(0) != 'a') { // offset for debugging
  //   init_x = 0.0, init_y = 0.0, init_z = 0.0;
  //   init_qx = 0.0, init_qy = 0.0, init_qz = sqrt(0.01), init_qw = sqrt(0.99);
  // }
  bool b_have_fiducial = true;
  if (!pu::Get("fiducial_calibration/position/x", init_x))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/position/y", init_y))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/position/z", init_z))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/orientation/x", init_qx))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/orientation/y", init_qy))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/orientation/z", init_qz))
    b_have_fiducial = false;
  if (!pu::Get("fiducial_calibration/orientation/w", init_qw))
    b_have_fiducial = false;

  if (!b_have_fiducial) {
    ROS_WARN("Can't find fiducials, using origin");
  }

  // Load initial position and orientation noise.
  double sigma_x = 0.0, sigma_y = 0.0, sigma_z = 0.0;
  double sigma_roll = 0.0, sigma_pitch = 0.0, sigma_yaw = 0.0;
  if (!pu::Get("init/position_sigma/x", sigma_x))
    return false;
  if (!pu::Get("init/position_sigma/y", sigma_y))
    return false;
  if (!pu::Get("init/position_sigma/z", sigma_z))
    return false;
  if (!pu::Get("init/orientation_sigma/roll", sigma_roll))
    return false;
  if (!pu::Get("init/orientation_sigma/pitch", sigma_pitch))
    return false;
  if (!pu::Get("init/orientation_sigma/yaw", sigma_yaw))
    return false;

  // convert initial quaternion to Roll/Pitch/Yaw
  double init_roll = 0.0, init_pitch = 0.0, init_yaw = 0.0;
  gu::Quat q(gu::Quat(init_qw, init_qx, init_qy, init_qz));
  gu::Rot3 m1;
  m1 = gu::QuatToR(q);
  init_roll = m1.Roll();
  init_pitch = m1.Pitch();
  init_yaw = m1.Yaw();

  // Set the initial position.
  Vector3 translation(init_x, init_y, init_z);
  Rot3 rotation(Rot3::RzRyRx(init_roll, init_pitch, init_yaw));
  Pose3 pose(rotation, translation);

  // Set the covariance on initial position.
  initial_noise_ << sigma_roll, sigma_pitch, sigma_yaw, sigma_x, sigma_y,
      sigma_z;

  gtsam::noiseModel::Diagonal::shared_ptr covariance(
      gtsam::noiseModel::Diagonal::Sigmas(initial_noise_));
  ROS_INFO_STREAM("covariance is");
  ROS_INFO_STREAM(initial_noise_);

  // Initialize graph  with the initial position
  InitializeGraph(pose, covariance);

  return true;
}

bool LampRobot::InitializeGraph(
    gtsam::Pose3& pose, gtsam::noiseModel::Diagonal::shared_ptr& covariance) {
  pose_graph_.Initialize(GetInitialKey(), pose, covariance);

  return true;
}

bool LampRobot::InitializeHandlers(const ros::NodeHandle& n) {
  if (!odometry_handler_.Initialize(n)) {
    ROS_ERROR("%s: Failed to initialize the odometry handler.", name_.c_str());
    return false;
  }

  if (!artifact_handler_.Initialize(n)) {
    ROS_ERROR("%s: Failed to initialize the artifact handler.", name_.c_str());
    return false;
  }

  if (!april_tag_handler_.Initialize(n)) {
    ROS_ERROR("%s: Failed to initialize the april tag handler.", name_.c_str());
    return false;
  }

  if (!uwb_handler_.Initialize(n)) {
    ROS_ERROR("%s: Failed to initialize the uwb handler.", name_.c_str());
    return false;
  }

  if (!imu_handler_.Initialize(n)) {
    ROS_ERROR("%s: Failed to initialize the imu handler.", name_.c_str());
    return false;
  }

  return true;
}

// Check for data from all of the handlers
bool LampRobot::CheckHandlers() {
  // b_has_new_factor_ will be set to true if there is a new factor
  // b_run_optimization_ will be set to true if there is a new loop closure

  bool b_have_odom_factors;
  // bool b_have_loop_closure;
  bool b_have_new_artifacts;
  bool b_have_new_april_tags;
  bool b_have_new_uwb;

  // Check the odom for adding new poses
  b_have_odom_factors = ProcessOdomData(odometry_handler_.GetData());

  // Check all handlers
  // Set the initialized flag in artifacts and april to start
  // receiving messages.
  if ((pose_graph_.GetValues().size() > 0) && (!is_artifact_initialized)) {
    is_artifact_initialized = true;
    artifact_handler_.SetPgoInitialized(true);
    april_tag_handler_.SetPgoInitialized(true);
  }
  // Check for artifacts
  b_have_new_artifacts = ProcessArtifactData(artifact_handler_.GetData());
  // Check for april tags
  b_have_new_april_tags = ProcessAprilTagData(april_tag_handler_.GetData());
  // Check for UWB data
  if (b_use_uwb_)
    b_have_new_uwb = ProcessUwbData(uwb_handler_.GetData());
  return true;
}

void LampRobot::ProcessTimerCallback(const ros::TimerEvent& ev) {
  // Print some debug messages
  // ROS_INFO_STREAM("Checking for new data");

  // Publish odom
  UpdateAndPublishOdom();

  // Check the handlers
  CheckHandlers();

  // Publish the pose graph
  if (b_has_new_factor_) {
    PublishPoseGraph();

    // Publish the full map (for debug)
    mapper_.PublishMap();

    b_has_new_factor_ = false;
  }

  // Start optimize, if needed
  if (b_run_optimization_) {
    ROS_INFO_STREAM(
        "Optimization activated: Publishing pose graph to optimizer");
    PublishPoseGraphForOptimizer();

    b_run_optimization_ = false;
  }

  // Publish anything that is needed
}

/*!
  \brief  Calls handler function to update global position of the artifacts
  \author Abhishek Thakur
  \date 09 Oct 2019
*/
void LampRobot::UpdateArtifactPositions() {
  // Get new positions of artifacts from the pose-graph for artifact_key
  std::unordered_map<long unsigned int, ArtifactInfo>& artifact_info_hash =
      artifact_handler_.GetArtifactKey2InfoHash();

  // Result of updating the global pose
  bool result;

  // Loop over to update global pose.
  for (auto const& it : artifact_info_hash) {
    // Get the key
    gtsam::Symbol artifact_key = gtsam::Symbol(it.first);

    // Get the pose from the pose graph
    gtsam::Point3 artifact_position =
        pose_graph_.GetPose(artifact_key).translation();

    // Update global pose just for what has changed. returns bool
    result = result ||
        artifact_handler_.UpdateGlobalPosition(artifact_key, artifact_position);
  }
}

//-------------------------------------------------------------------

// Handler Wrappers
/*!
  \brief  Wrapper for the odom class interactions
  Creates factors from the odom output
  \param   data - the output data struct from the OdometryHandler class
  \warning ...
  \author Benjamin Morrell
  \date 01 Oct 2019
*/
bool LampRobot::ProcessOdomData(std::shared_ptr<FactorData> data) {
  // Extract odom data
  std::shared_ptr<OdomData> odom_data =
      std::dynamic_pointer_cast<OdomData>(data);

  // Check if there are new factors
  if (!odom_data->b_has_data) {
    return false;
  }

  // Record new factor being added - need to publish pose graph
  b_has_new_factor_ = true;

  // process data for each new factor
  for (auto odom_factor : odom_data->factors) {
    ROS_INFO("Adding new odom factor to pose graph");
    // Get the transforms - odom transforms
    Pose3 transform = odom_factor.transform;
    gtsam::SharedNoiseModel covariance =
        odom_factor.covariance; // TODO - check format

    if (b_use_fixed_covariances_) {
      covariance = SetFixedNoiseModels("odom");
    }

    std::pair<ros::Time, ros::Time> times = odom_factor.stamps;

    // Get the previous key - special case for odom that we use key)
    Symbol prev_key = pose_graph_.key - 1;
    // Get the current "new key" value stored in the variable pose_graph_.key
    Symbol current_key = pose_graph_.key;
    // Increment key
    pose_graph_.key = pose_graph_.key + 1;

    // TODO - use this for other handlers: Symbol prev_key =
    // GetKeyAtTime(times.first);

    // Compute the new value with a normalized transform
    Pose3 last_pose = pose_graph_.GetPose(prev_key);
    ROS_INFO_STREAM("Last pose det: " << last_pose.rotation().matrix().determinant());
    Eigen::Quaterniond quat(last_pose.rotation().matrix());
    quat = quat.normalized();
    last_pose = Pose3(gtsam::Rot3(quat.toRotationMatrix()), last_pose.translation());
    ROS_INFO_STREAM("Last pose det after: " << last_pose.rotation().matrix().determinant());


    // Add values to graph so have it for adding map TODO - use unit covariance
    pose_graph_.TrackNode(
        times.second, current_key, last_pose.compose(transform), covariance);

    // add  node/keyframe to keyed stamps
    pose_graph_.InsertKeyedStamp(current_key, times.second);
    pose_graph_.InsertStampedOdomKey(times.second.toSec(), current_key);

    // Track the edges that have been added
    int type = pose_graph_msgs::PoseGraphEdge::ODOM;
    pose_graph_.TrackFactor(prev_key, current_key, type, transform, covariance);

    if (b_add_imu_factors_){
      imu_handler_.SetTimeForImuAttitude(times.second);
      imu_handler_.SetKeyForImuAttitude(current_key);
      ProcessImuData(imu_handler_.GetData());
    }

    // Get keyed scan from odom handler
    PointCloud::Ptr new_scan(new PointCloud);

    if (odom_factor.b_has_point_cloud) {
      // Store the keyed scan and add it to the map

      // Copy input scan.
      PointCloud::Ptr new_scan;
      new_scan = odom_factor.point_cloud;

      // // TODO: Make this a tunable parameter
      const int n_points = static_cast<int>(
          (1.0 - params_.decimate_percentage) * new_scan->size());

      // // Apply random downsampling to the keyed scan
      pcl::RandomSample<pcl::PointXYZI> random_filter;
      random_filter.setSample(n_points);
      random_filter.setInputCloud(new_scan);
      random_filter.filter(*new_scan);

      // Apply voxel grid filter to the keyed scan
      pcl::VoxelGrid<pcl::PointXYZI> grid;
      grid.setLeafSize(params_.grid_res, params_.grid_res, params_.grid_res);
      grid.setInputCloud(new_scan);
      grid.filter(*new_scan);

      pose_graph_.InsertKeyedScan(current_key, new_scan);

      AddTransformedPointCloudToMap(current_key);

      // publish keyed scan
      pose_graph_msgs::KeyedScan keyed_scan_msg;
      keyed_scan_msg.key = current_key;
      pcl::toROSMsg(*new_scan, keyed_scan_msg.scan);
      keyed_scan_pub_.publish(keyed_scan_msg);
    }
  }

  return true;
}

// Odometry update
void LampRobot::UpdateAndPublishOdom() {
  // Get the pose at the last key
  Pose3 last_pose = pose_graph_.LastPose();

  // Get the delta from the last pose to now
  ros::Time stamp; // = ros::Time::now();
  GtsamPosCov delta_pose_cov;
  // if (!odometry_handler_.GetOdomDelta(stamp, delta_pose_cov)) {
  // Had a bad odom return - try latest time from odometry_handler
  if (!odometry_handler_.GetOdomDeltaLatestTime(stamp, delta_pose_cov)) {
    ROS_WARN("No good velocity output yet");
    // TODO - work out what the best thing is to do in this scenario
    return;
  }
  // ROS_INFO("Got good result from getting delta at the latest time");
  // }

  // odometry_handler_.GetDeltaBetweenTimes(keyed_stamps_[key_ - 1], stamp,
  // delta_pose);

  // Compose the delta
  auto delta_pose = delta_pose_cov.pose;
  auto delta_cov = delta_pose_cov.covariance;

  Pose3 new_pose = last_pose.compose(delta_pose);

  // TODO use the covariance when we have it
  // gtsam::Matrix66 covariance;
  // odometry_handler_.GetDeltaCovarianceBetweenTimes(pose_graph_.keyed_stamps[pose_graph_.key-1],
  // stamp, covariance);
  //
  // Compose covariance
  // TODO

  // Convert to ROS to publish
  geometry_msgs::PoseStamped msg;
  msg.pose = utils::GtsamToRosMsg(new_pose);
  msg.header.frame_id = pose_graph_.fixed_frame_id;
  msg.header.stamp = stamp;

  // TODO - use the covariance when we have it
  // geometry_msgs::PoseWithCovarianceStamped msg;
  // msg.pose = utils::GtsamToRosMsg(new_pose, covariance);
  // msg.header.frame_id = pose_graph_.fixed_frame_id;
  // msg.header.stamp = stamp;

  // Publish pose graph
  pose_pub_.publish(msg);
}

/*!
  \brief  Wrapper for the imu class interactions
  Creates factors from the imu output - called when there is a new odom message
  \param   data - the output data struct from the ImuHandler class
  \warning ...time sync
  \author Benjamin Morrell
  \date 22 Nov 2019
*/
bool LampRobot::ProcessImuData(std::shared_ptr<FactorData> data) {
  // Extract odom data
  std::shared_ptr<ImuData> imu_data =
      std::dynamic_pointer_cast<ImuData>(data);

  // Check if there are new factors
  if (!imu_data->b_has_data) {
    return false;
  }

  Unit3 meas_unit = imu_data->factors[0].attitude.nZ();
  geometry_msgs::Point meas;
  meas.x = meas_unit.point3().x();
  meas.y = meas_unit.point3().y();
  meas.z = meas_unit.point3().z();

  // gtsam::noiseModel::Isotropic noise = boost::dynamic_pointer_cast<gtsam::noiseModel::Isotropic>(imu_data->factors[0].attitude.noiseModel());
  double noise_sigma =
        boost::dynamic_pointer_cast<gtsam::noiseModel::Isotropic>(
            imu_data->factors[0].attitude.noiseModel())
            ->sigma();

  pose_graph_.TrackIMUFactor(imu_data->factors[0].attitude.front(),
                               meas,
                               noise_sigma,
                               true);

  // Optimize every "imu_factors_per_opt"
  imu_factor_count_++;

  if (imu_factor_count_ % imu_factors_per_opt_ == 0){
    b_run_optimization_ = true;
  }

  return true;

}

/*!
  \brief  Wrapper for the artifact class interactions
  Creates factors from the artifact output
  \param   data - the output data struct from the ArtifactHandler class
  \warning ...
  \author Abhishek Thakur
  \date 08 Oct 2019
*/
bool LampRobot::ProcessArtifactData(std::shared_ptr<FactorData> data) {
  // Extract artifact data
  std::shared_ptr<ArtifactData> artifact_data =
      std::dynamic_pointer_cast<ArtifactData>(data);

  // Check if there are new factors
  if (!artifact_data->b_has_data) {
    return false;
  }

  b_has_new_factor_ = true;

  // Necessary variables
  Pose3 transform;
  Pose3 temp_transform;
  Pose3 global_pose;
  gtsam::SharedNoiseModel covariance;
  ros::Time timestamp;
  gtsam::Symbol pose_key;
  gtsam::Symbol cur_artifact_key;

  // process data for each new factor
  for (auto artifact : artifact_data->factors) {
    // Get the time
    timestamp = artifact.stamp;

    // Get the artifact key
    cur_artifact_key = artifact.key;

    // Get the pose measurement
    if (b_artifacts_in_global_) {
      // Convert pose to relative frame
      if (!ConvertGlobalToRelative(
              timestamp,
              gtsam::Pose3(gtsam::Rot3(), artifact.position),
              temp_transform)) {
        ROS_ERROR("Can't convert artifact from global to relative");
        b_has_new_factor_ = false;
        // Clean Artifact handler so that these set of
        // factors are removed from history
        artifact_handler_.CleanFailedFactors(false);
        return false;
      }
    } else {
      // Is in relative already
      ROS_INFO("Have artifact in relative frame");
      temp_transform = gtsam::Pose3(gtsam::Rot3(), artifact.position);
    }

    // Is a relative tranform, so need to handle linking to the pose-graph
    HandleRelativePoseMeasurement(
        timestamp, temp_transform, transform, global_pose, pose_key);

    if (pose_key == utils::GTSAM_ERROR_SYMBOL) {
      ROS_ERROR("Bad artifact time. Not adding to graph - ERROR THAT NEEDS TO "
                "BE HANDLED OR LOSE ARTIFACTS!!");
      b_has_new_factor_ = false;
      // Clean Artifact handler so that these set of
      // factors are removed from history
      artifact_handler_.CleanFailedFactors(false);
      return false;
    }

    // Get the covariances (Should be in relative frame as well)
    // TODO - handle this better - need to add covariances from the odom - do in
    // the function above
    covariance = artifact.covariance;

    if (b_use_fixed_covariances_) {
      covariance = SetFixedNoiseModels("artifact");
    }

    // Check if it is a new artifact or not
    if (!pose_graph_.HasKey(cur_artifact_key)) {
      ROS_INFO("Have a new artifact in LAMP");

      // Insert into the values TODO - add unit covariance
      std::string id = artifact_handler_.GetArtifactID(cur_artifact_key);
      pose_graph_.TrackNode(
          timestamp, cur_artifact_key, global_pose, covariance, id);

      // Add keyed stamps
      pose_graph_.InsertKeyedStamp(cur_artifact_key, timestamp);

      // Publish the new artifact, with the global pose
      ROS_INFO("Calling Publish Artifacts");
      artifact_handler_.PublishArtifacts(cur_artifact_key, global_pose);

    } else {
      // Second sighting of an artifact - we have a loop closure
      ROS_INFO_STREAM("Artifact re-sighted with key: "
                      << gtsam::DefaultKeyFormatter(cur_artifact_key));
      b_run_optimization_ = true;
    }

    // Add and track the edges that have been added
    int type = pose_graph_msgs::PoseGraphEdge::ARTIFACT;
    pose_graph_.TrackFactor(
        pose_key, cur_artifact_key, type, transform, covariance);
    ROS_INFO("Added artifact to pose graph factors in lamp");
  }

  ROS_INFO("Successfully complete ArtifactProcess call with an artifact");

  // Clean up for next iteration
  artifact_handler_.CleanFailedFactors(true);
  return true;
}

/*!
  \brief  Wrapper for the april tag class interactions
  Creates factors from the april tag output
  \param   data - the output data struct from the AprilTagHandler class
  \warning ...
  \author Abhishek Thakur
  \date Oct 2019
*/
bool LampRobot::ProcessAprilTagData(std::shared_ptr<FactorData> data) {
  // Extract artifact data
  std::shared_ptr<AprilTagData> april_tag_data =
      std::dynamic_pointer_cast<AprilTagData>(data);

  // Check if there are new factors
  if (!april_tag_data->b_has_data) {
    return false;
  }

  b_has_new_factor_ = true;

  // Necessary variables
  Pose3 transform;
  Pose3 temp_transform;
  Pose3 global_pose;
  gtsam::SharedNoiseModel covariance;
  ros::Time timestamp;
  gtsam::Symbol pose_key;
  gtsam::Symbol cur_april_tag_key;

  // process data for each new factor
  for (auto april_tag : april_tag_data->factors) {
    // Get the time
    timestamp = april_tag.stamp;

    // Get the april tag key
    cur_april_tag_key = april_tag.key;

    // Get the ground truth data using april tag key in info hashmap.
    // TODO: Usefulness of ground truth in april tag factor
    // Currently using ground truth from artifactkey2infohash
    gtsam::Pose3 ground_truth =
        april_tag_handler_.GetGroundTruthData(cur_april_tag_key);

    // Get the pose measurement
    if (b_artifacts_in_global_) {
      // Convert pose to relative frame
      if (!ConvertGlobalToRelative(
              timestamp,
              gtsam::Pose3(gtsam::Rot3(), april_tag.position),
              temp_transform)) {
        ROS_ERROR("Can't convert April tag from global to relative");
        b_has_new_factor_ = false;

        // Clean all the new factors
        april_tag_handler_.CleanFailedFactors(false);
        return false;
      }
    } else {
      // Is in relative already
      ROS_INFO("Have April tag in relative frame");
      temp_transform = gtsam::Pose3(gtsam::Rot3(), april_tag.position);
    }

    // Is a relative tranform, so need to handle linking to the pose-graph
    HandleRelativePoseMeasurement(
        timestamp, temp_transform, transform, global_pose, pose_key);

    if (pose_key == utils::GTSAM_ERROR_SYMBOL) {
      ROS_ERROR("Bad april tag time. Not adding to graph - ERROR THAT NEEDS TO "
                "BE HANDLED OR LOSE APRIL TAG!!");
      b_has_new_factor_ = false;

      // Clean all the new factors
      april_tag_handler_.CleanFailedFactors(false);

      return false;
    }

    // Get the covariances (Should be in relative frame as well)
    // TODO - handle this better - need to add covariances from the odom - do in
    // the function above
    covariance = april_tag.covariance;

    if (b_use_fixed_covariances_) {
      covariance = SetFixedNoiseModels("april");
    }

    // Check if it is a new april tag or not
    if (!pose_graph_.HasKey(cur_april_tag_key)) {
      ROS_INFO("Have a new April Tag in LAMP");

      // Insert into the values. TODO - use correct covariance
      pose_graph_.TrackNode(
          timestamp, cur_april_tag_key, global_pose, covariance);

      // Add keyed stamps
      pose_graph_.InsertKeyedStamp(cur_april_tag_key, timestamp);

      // Get the noise in ground truth data
      gtsam::SharedNoiseModel noise = SetFixedNoiseModels("april");

      // Add and track the prior factor if its a new april tag.
      ROS_INFO("Adding prior factor for April Tag");
      pose_graph_.TrackPrior(cur_april_tag_key, ground_truth, noise);
    } else {
      // Second sighting of an april tag - we have a loop closure
      ROS_INFO_STREAM("April tag re-sighted with key: "
                      << gtsam::DefaultKeyFormatter(cur_april_tag_key));
    }
    // Since a factor is added in any case new or seen, run optimization
    b_run_optimization_ = true;

    // Add and track the edges that have been added
    int type = pose_graph_msgs::PoseGraphEdge::ARTIFACT;
    pose_graph_.TrackFactor(
        pose_key, cur_april_tag_key, type, transform, covariance);
    ROS_INFO("Added April Tag to pose graph factors in lamp");
  }

  ROS_INFO("Successfully completed ProcessAprilTagData call with an April Tag");

  // Clean the new keys
  april_tag_handler_.CleanFailedFactors(true);

  return true;
}

/*!
  \brief  Wrapper for the uwb class interactions
  Creates factors from the uwb output
  \param   data - the output data struct from the UwbHandler class
  \warning ...
  \author Nobuhiro Funabiki
  \date Oct 2019
*/
bool LampRobot::ProcessUwbData(std::shared_ptr<FactorData> data) {
  std::shared_ptr<UwbData> uwb_data = std::dynamic_pointer_cast<UwbData>(data);
  if (!uwb_data->b_has_data)
    return false;
  // New Factors to be added
  NonlinearFactorGraph new_factors;
  Pose3 global_uwb_pose; // TODO: How to initialize the pose of UWB node?

  ROS_INFO_STREAM("UWB ID to be added : u" << uwb_data->factors.at(0).key_to);
  ROS_INFO_STREAM("Number of UWB factors to be added : " << uwb_data->factors.size());

  for (auto factor : uwb_data->factors) {
    auto odom_key = factor.key_from;
    auto uwb_key = gtsam::Symbol('u', factor.key_to);

    // Check if it is a new uwb id or not
    auto values = pose_graph_.GetValues();

    if (!values.exists(uwb_key) && factor.type != pose_graph_msgs::PoseGraphEdge::UWB_BETWEEN) {
      // Insert it into the values
      global_uwb_pose = pose_graph_.GetPose(odom_key);
      // Add it into the keyed stamps
      pose_graph_.InsertKeyedStamp(uwb_key, factor.stamp);
      // TODO: SetFixedNoiseModels should be used for the following sentences
      gtsam::Vector6 prior_precision;
      prior_precision.head<3>().setConstant(0.0000001);
      prior_precision.tail<3>().setConstant(0.0000001);
      static const gtsam::SharedNoiseModel& prior_noise =
          gtsam::noiseModel::Diagonal::Precisions(prior_precision);
      pose_graph_.TrackNode(
          factor.stamp, uwb_key, global_uwb_pose, prior_noise);
    }

    if (factor.type == pose_graph_msgs::PoseGraphEdge::UWB_RANGE) {
      ROS_INFO("Adding a UWB range factor");
      auto range = factor.range;
      gtsam::noiseModel::Base::shared_ptr range_error =
          gtsam::noiseModel::Isotropic::Sigma(1, uwb_range_sigma_);
      new_factors.add(gtsam::RangeFactor<Pose3, Pose3>(
          odom_key, uwb_key, range, range_error));
      // Track the edges that have been added
      EdgeMessage uwb_factor;
      uwb_factor.key_from = odom_key;
      uwb_factor.key_to = uwb_key;
      uwb_factor.type = pose_graph_msgs::PoseGraphEdge::UWB_RANGE;
      uwb_factor.range = range;
      uwb_factor.range_error = uwb_range_sigma_;
      // Add new factors to buffer to send to pgo
      pose_graph_.TrackFactor(uwb_factor);
    }
    else if (factor.type == pose_graph_msgs::PoseGraphEdge::UWB_BETWEEN) {
      ROS_INFO("Adding a UWB between factor");
      auto odom_pose = pose_graph_.GetPose(odom_key);
      auto dropped_relative_pose = factor.pose;
      dropped_relative_pose.print("dropped_relative_pose");
      auto global_uwb_pose = odom_pose.compose(dropped_relative_pose);
      gtsam::Vector6 sigmas;
      sigmas.head<3>().setConstant(uwb_between_rot_sigma_);
      sigmas.tail<3>().setConstant(uwb_between_trans_sigma_);
      auto noise = gtsam::noiseModel::Diagonal::Sigmas(sigmas);
      pose_graph_.TrackNode(
          factor.stamp, uwb_key, global_uwb_pose, noise);
      // Add new factors to buffer to send to pgo
      pose_graph_.TrackFactor(
        odom_key,
        uwb_key,
        pose_graph_msgs::PoseGraphEdge::UWB_BETWEEN,
        dropped_relative_pose,
        noise,
        true
      );
    }
    
  }
  b_run_optimization_ = true;
  uwb_handler_.ResetFactorData();
  return true;
}

// Function gets a relative pose and time, and returns the global pose and the
// transform from the closest node in time, as well as the key of the closest
// node
/*!
  \brief  Function to handle relative measurements and adding them to the
  pose-graph \param   stamp          - The time of the measurement \param
  relative_pose  - The observed relative pose \param   transform      - The
  output transform (for a between factor) \param   global pose    - The output
  global pose estimate \param   key_from       - The output key from which the
  new relative measurement is attached \warning ... \author Benjamin Morrell
  \date 01 Oct 2019
*/
void LampRobot::HandleRelativePoseMeasurement(const ros::Time& stamp,
                                              const gtsam::Pose3& relative_pose,
                                              gtsam::Pose3& transform,
                                              gtsam::Pose3& global_pose,
                                              gtsam::Symbol& key_from) {
  // Get the key from:
  key_from = pose_graph_.GetClosestKeyAtTime(stamp, false);

  if (key_from == utils::GTSAM_ERROR_SYMBOL) {
    ROS_ERROR("Measurement is from a time out of range. Rejecting");
    return;
  }

  // Time from this key - closest time that there is anode
  ros::Time stamp_from = pose_graph_.keyed_stamps[key_from];

  // Get the delta pose from the key_from to the time of the observation
  GtsamPosCov delta_pose_cov;
  delta_pose_cov =
      odometry_handler_.GetFusedOdomDeltaBetweenTimes(stamp_from, stamp);

  if (!delta_pose_cov.b_has_value) {
    ROS_ERROR("----------Could not get delta between times - THIS CASE IS NOT "
              "WELL HANDLED YET-----------");
    key_from = utils::GTSAM_ERROR_SYMBOL;
    return;
  }

  // TODO - do covariances as well

  // Compose the transforms to get the between factor
  gtsam::Pose3 delta_pose = delta_pose_cov.pose;
  gtsam::SharedNoiseModel delta_cov = delta_pose_cov.covariance;
  transform = delta_pose.compose(relative_pose);

  // Compose from the node in the graph to get the global position
  // TODO - maybe do this outside this function
  global_pose = pose_graph_.GetPose(key_from).compose(transform);
}

// Placeholder function for handling global artifacts (will soon be relative)
bool LampRobot::ConvertGlobalToRelative(const ros::Time stamp,
                                        const gtsam::Pose3 pose_global,
                                        gtsam::Pose3& pose_relative) {
  // Get the closes node in the pose-graph
  gtsam::Symbol key_from = pose_graph_.GetClosestKeyAtTime(stamp, false);

  if (key_from == utils::GTSAM_ERROR_SYMBOL) {
    ROS_ERROR(
        "Key from artifact key_from is outside of range - can't link artifact");
    return false;
  }

  // Pose of closest node
  gtsam::Pose3 node_pose = pose_graph_.GetPose(key_from);

  // Compose to get the relative position (relative pose between node and
  // global)
  pose_relative = node_pose.between(pose_global);

  return true;
}

// TODO Function handler wrappers
// - hopefully a lot of cutting code from others

// TODO
// - Unit tests for these functions
// - How to handle relative measurements not directly at nodes

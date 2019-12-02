/*
 * Copyright Notes
 *
 * Authors: Benjamin Morrell    (benjamin.morrell@jpl.nasa.gov)
 */


// Includes
#include <lamp/LampBaseStation.h>

// #include <math.h>
// #include <ctime>

namespace pu = parameter_utils;
namespace gu = geometry_utils;

// Constructor (if there is override)
LampBaseStation::LampBaseStation(): 
        b_published_initial_node_(false) {

  // On base station LAMP, republish values after optimization
  b_repub_values_after_optimization_ = true;
}

//Destructor
LampBaseStation::~LampBaseStation() {}

// Initialization - override for Base Station Setup
bool LampBaseStation::Initialize(const ros::NodeHandle& n, bool from_log) {

  // Get the name of the process
  name_ = ros::names::append(n.getNamespace(), "LampBaseStation");

  if (!mapper_.Initialize(n)) {
    ROS_ERROR("%s: Failed to initialize mapper.", name_.c_str());
    return false;
  }

  // Add load params etc
  if (!LoadParameters(n)) {
    ROS_ERROR("%s: Failed to load parameters.", name_.c_str());
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

  // Init Handlers
  if (!InitializeHandlers(n)){
    ROS_ERROR("%s: Failed to initialize handlers.", name_.c_str());
    return false;
  }
}

bool LampBaseStation::LoadParameters(const ros::NodeHandle& n) {


  if (!pu::Get("base/b_optimize_on_artifacts", b_optimize_on_artifacts_)) {
    return false;
  }

  // Names of all robots for base station to subscribe to
  if (!pu::Get("robot_names", robot_names_)) {
  ROS_ERROR("%s: No robot names provided to base station.", name_.c_str());
    return false;
  }
  else {
    for (auto s : robot_names_) {
      ROS_INFO_STREAM("Registered new robot: " << s);
    }
  }

  // TODO : separate rate for base and robot
  if (!pu::Get("rate/update_rate", update_rate_))
    return false;

  // Fixed precisions
  // TODO - eventually remove the need to use this
  if (!SetFactorPrecisions()) {
    ROS_ERROR("SetFactorPrecisions failed");
    return false;
  }

  // Initialize frame IDs
  pose_graph_.fixed_frame_id = "world";

  // Initialize booleans
  b_run_optimization_ = false;
  b_has_new_factor_ = false;
  b_has_new_scan_ = false;


  return true;
}

bool LampBaseStation::RegisterCallbacks(const ros::NodeHandle& n) {

  // Create a local nodehandle to manage callback subscriptions.
  ros::NodeHandle nl(n);

  // Create the timed callback
  update_timer_ =
      nl.createTimer(update_rate_, &LampBaseStation::ProcessTimerCallback, this);

  back_end_pose_graph_sub_ = nl.subscribe("optimized_values",
                                          1,
                                          &LampBaseStation::OptimizerUpdateCallback,
                                          dynamic_cast<LampBase*>(this));

  laser_loop_closure_sub_ = nl.subscribe("laser_loop_closures",
                                         1,
                                         &LampBaseStation::LaserLoopClosureCallback,
                                         dynamic_cast<LampBase*>(this));

  // Uncomment when needed for debugging
  debug_sub_ = nl.subscribe("debug",
                            1,
                            &LampBaseStation::DebugCallback,
                            this);

  return true; 
}

bool LampBaseStation::CreatePublishers(const ros::NodeHandle& n) {

  // Creates pose graph publishers in base class
  LampBase::CreatePublishers(n);

  // Create a local nodehandle to manage callback subscriptions.
  ros::NodeHandle nl(n);

  // Base station publishers
  pose_graph_to_optimize_pub_ = nl.advertise<pose_graph_msgs::PoseGraph>("pose_graph_to_optimize", 10, false);

  return true; 
}

bool LampBaseStation::InitializeHandlers(const ros::NodeHandle& n){
  
  // Manual loop closure handler
  if (!manual_loop_closure_handler_.Initialize(n)) {
    ROS_ERROR("%s: Failed to initialize the manual loop closure handler.", name_.c_str());
    return false;
  }

  // Pose graph handler -- requires robot_names parameter loaded
  if (robot_names_.size() == 0) {
    ROS_ERROR("%s: No robots for base station to subscribe to.", name_.c_str());
    return false;
  }
  else if (!pose_graph_handler_.Initialize(n, robot_names_)) {
    ROS_ERROR("%s: Failed to initialize the pose graph handler.", name_.c_str());
    return false;
  }
  
  return true; 
}

void LampBaseStation::ProcessTimerCallback(const ros::TimerEvent& ev) {

  // Check the handlers
  CheckHandlers();

  // Send data to optimizer - pose graph and map publishing happens in 
  // callback when data is received back from optimizer
  if (b_run_optimization_) {
    ROS_INFO_STREAM("Publishing pose graph to optimizer");
    PublishPoseGraphForOptimizer();

    b_run_optimization_ = false; 
  }

  if (b_has_new_factor_) {
    PublishPoseGraph();

    b_has_new_factor_ = false;
  }

  if (b_has_new_scan_) {
    mapper_.PublishMap();

    b_has_new_scan_ = false;
  }

  // Publish anything that is needed 
}

bool LampBaseStation::ProcessPoseGraphData(std::shared_ptr<FactorData> data) {
  // ROS_INFO_STREAM("In ProcessPoseGraphData");

  // Extract pose graph data
  std::shared_ptr<PoseGraphData> pose_graph_data = std::dynamic_pointer_cast<PoseGraphData>(data);

  // Check if there are new pose graphs
  if (!pose_graph_data->b_has_data) {
    return false; 
  }

  ROS_INFO_STREAM("New data received at base: " << pose_graph_data->graphs.size() <<
   " graphs, " << pose_graph_data->scans.size() << " scans ");
  b_has_new_factor_ = true;

  pose_graph_msgs::PoseGraph::Ptr graph_ptr; 
  pcl::PointCloud<pcl::PointXYZI>::Ptr scan_ptr(new pcl::PointCloud<pcl::PointXYZI>);

  gtsam::Values new_values;

  for (auto g : pose_graph_data->graphs) {

    // Register new data - this will cause pose graph to publish
    b_has_new_factor_ = true;

    // Merge the current (slow) graph before processing incoming graphs
    merger_.OnSlowGraphMsg(pose_graph_.ToMsg());
    merger_.OnFastGraphMsg(g);

    // Update the internal pose graph using the merger output
    pose_graph_msgs::PoseGraphConstPtr fused_graph(
    new pose_graph_msgs::PoseGraph(merger_.GetCurrentGraph()));
    pose_graph_.UpdateFromMsg(fused_graph);

    // Check for new loop closure edges
    for (pose_graph_msgs::PoseGraphEdge e : g->edges) {
      if (e.type == pose_graph_msgs::PoseGraphEdge::LOOPCLOSE) {
      
        // Run optimization to update the base station graph afterwards
        b_run_optimization_ = true;
      }

      // Optimize on artifact loop closures (currently optimizes on all artifact edges)
      if (b_optimize_on_artifacts_ && e.type == pose_graph_msgs::PoseGraphEdge::ARTIFACT) {
      
        // Run optimization to update the base station graph afterwards
        b_run_optimization_ = true;
      }

    }

    ROS_INFO_STREAM("Added new pose graph");
  }

  ROS_INFO_STREAM("Keyed stamps: " << pose_graph_.keyed_stamps.size());

  // Update from stored keyed scans 
  for (auto s : pose_graph_data->scans) {

    // Register new data - this will cause map to publish
    b_has_new_scan_ = true;

    pcl::fromROSMsg(s->scan, *scan_ptr);
    pose_graph_.InsertKeyedScan(s->key, scan_ptr); // TODO: add overloaded function
    AddTransformedPointCloudToMap(s->key);

    ROS_INFO_STREAM("Added new point cloud to map, " << scan_ptr->points.size() << " points");
  }

  return true; 
}

bool LampBaseStation::ProcessManualLoopClosureData(std::shared_ptr<FactorData> data) {

  // Extract loop closure data
  std::shared_ptr<LoopClosureData> manual_loop_closure_data = std::dynamic_pointer_cast<LoopClosureData>(data);

  if (!manual_loop_closure_data->b_has_data) {
    return false;
  }

  ROS_INFO_STREAM("Received new manual loop closure data");

  for (auto factor : manual_loop_closure_data->factors) {
    pose_graph_.TrackFactor(factor.key_from, 
                            factor.key_to,
                            pose_graph_msgs::PoseGraphEdge::LOOPCLOSE, 
                            factor.transform, 
                            factor.covariance);

    b_run_optimization_ = true;
  }

  return true;
}


// Check for data from all of the handlers
bool LampBaseStation::CheckHandlers() {

  // Check for pose graphs from the robots
  ProcessPoseGraphData(pose_graph_handler_.GetData());

  // Check for manual loop closures
  ProcessManualLoopClosureData(manual_loop_closure_handler_.GetData());

  return true;
}

bool LampBaseStation::ProcessArtifactGT() {

  // Read from file
  if (!pu::Get("artifacts_GT", artifact_GT_strings_)) {
    ROS_ERROR("%s: No artifact ground truth data provided.", name_.c_str());
    return false;
  }

  for (auto s : artifact_GT_strings_) {
    ROS_INFO_STREAM("New artifact ground truth");
    artifact_GT_.push_back(ArtifactGroundTruth(s));

    ROS_INFO_STREAM("\t" << gtsam::DefaultKeyFormatter(artifact_GT_.back().key));
    ROS_INFO_STREAM("\t" << artifact_GT_.back().type);
    ROS_INFO_STREAM("\t" << artifact_GT_.back().position.x() << ", "<< artifact_GT_.back().position.y() << ", "<< artifact_GT_.back().position.z());
  }

  for (auto a : artifact_GT_) {

    if (!pose_graph_.HasKey(a.key)) {
      ROS_WARN_STREAM("Unable to add artifact ground truth for key " << gtsam::DefaultKeyFormatter(a.key));
      continue;
    }

    // Add the prior
    pose_graph_.TrackPrior(a.key, 
                           gtsam::Pose3(gtsam::Rot3(),a.position), 
                           SetFixedNoiseModels("artifact_gt"));


    // Trigger optimisation 
    b_run_optimization_ = true; 
  }

  return true;
}

void LampBaseStation::DebugCallback(const std_msgs::String msg) {
  ROS_INFO_STREAM("Debug message received: ");

  // Split message data into a vector of space-separated strings
  std::vector<std::string> data;
  boost::split(data, msg.data, [](char c){return c == ' ';}); 

  if (data.size() == 0) {
    ROS_INFO_STREAM("Invalid debug message data");
  }
  std::string cmd = data[0];

  // Freeze the current point cloud map on the visualizer
  if (cmd == "freeze") {
    ROS_INFO_STREAM("Publishing frozen map");
    mapper_.PublishMapFrozen();
  }

  // Read in artifact ground truth data
  else if (cmd == "artifact_gt") {
    ROS_INFO_STREAM("Processing artifact ground truth data");
    ProcessArtifactGT();
  }

  // Save the pose graph 
  else if (cmd == "save") {
    ROS_INFO_STREAM("Saving the pose graph");

    // Use filename if provided
    if (data.size() >= 2) {
      pose_graph_.Save(data[1]);
    }
    else {
      pose_graph_.Save("saved_pose_graph.zip");
    }
  }

  // Load pose graph from file 
  else if (cmd == "load") {
    ROS_INFO_STREAM("Loading pose graph and keyed scans");

    // Use filename if provided
    if (data.size() >= 2) {
      pose_graph_.Load(data[1]);
    }
    else {
      pose_graph_.Load("saved_pose_graph.zip");
    }
    
    PublishPoseGraph(); 
    ReGenerateMapPointCloud();
  }

  else {
    ROS_WARN_STREAM("Debug message not recognized");
  }

}

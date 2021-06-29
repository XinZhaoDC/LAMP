/**
 *  @brief Testing the Loop Closure Computation classes
 *
 */

#include <geometry_utils/Transform3.h>
#include <gtest/gtest.h>

#include "loop_closure/IcpLoopComputation.h"
#include "loop_closure/LoopComputation.h"

#include "test_artifacts.h"

namespace lamp_loop_closure {
class TestLoopComputation : public ::testing::Test {
 protected:
  TestLoopComputation() {
    // Load params
    system("rosparam load $(rospack find lamp)/config/lamp_settings.yaml");
    system(
        "rosparam load $(rospack find "
        "lamp)/config/precision_parameters.yaml");
    system(
        "rosparam load $(rospack find "
        "loop_closure)/config/laser_parameters.yaml");
  }
  ~TestLoopComputation() {}

  void computeTransforms() { icp_compute_.ComputeTransforms(); }

  void keyedScanCallback(const pose_graph_msgs::KeyedScan::ConstPtr& scan_msg) {
    icp_compute_.KeyedScanCallback(scan_msg);
  }

  void keyedPoseCallback(
      const pose_graph_msgs::PoseGraph::ConstPtr& graph_msg) {
    icp_compute_.KeyedPoseCallback(graph_msg);
  }

  bool performAlignment(const gtsam::Symbol& key1,
                        const gtsam::Symbol& key2,
                        const gtsam::Pose3& pose1,
                        const gtsam::Pose3& pose2,
                        geometry_utils::Transform3* delta,
                        gtsam::Matrix66* covariance) {
    return icp_compute_.PerformAlignment(
        key1, key2, pose1, pose2, delta, covariance);
  }

  void getSacInitialAlignment(PointCloud::ConstPtr source,
                              PointCloud::ConstPtr target,
                              Eigen::Matrix4f* tf_out,
                              double& sac_fitness_score) {
    icp_compute_.GetSacInitialAlignment(
        source, target, tf_out, sac_fitness_score);
  }

  void getTeaserInitialAlignment(PointCloud::ConstPtr source,
                                 PointCloud::ConstPtr target,
                                 Eigen::Matrix4f* tf_out,
                                 int& n_inliers) {
    icp_compute_.GetTeaserInitialAlignment(source, target, tf_out, n_inliers);
  }

  IcpLoopComputation icp_compute_;
  double tolerance_ = 1e-5;
};

TEST_F(TestLoopComputation, TestInitialize) {
  ros::NodeHandle nh;
  bool init = icp_compute_.Initialize(nh);
  ASSERT_TRUE(init);
}

TEST_F(TestLoopComputation, TestSacInitialAlign) {
  ros::NodeHandle nh;
  icp_compute_.Initialize(nh);

  // Add some keyed scans
  PointCloud::Ptr corner(new PointCloud);
  PointCloud::Ptr box(new PointCloud);
  corner = GenerateCorner();
  // Perturb a bit
  PointCloud::Ptr corner_moved(new PointCloud);
  Eigen::Matrix4f T = Eigen::Matrix4f::Identity();
  T(0, 3) = 1;
  T(1, 3) = -0.001;
  pcl::transformPointCloudWithNormals(*corner, *corner_moved, T, true);

  Eigen::Matrix4f T_est;
  double fitness_score;
  getSacInitialAlignment(corner, corner_moved, &T_est, fitness_score);

  EXPECT_TRUE(T.isApprox(T_est));
  EXPECT_EQ(0, fitness_score);
}

// TODO: Fix teaser unittests
// TEST_F(TestLoopComputation, TestTeaserInitialAlign) {
//   ros::NodeHandle nh;
//   icp_compute_.Initialize(nh);

//   // Add some keyed scans
//   PointCloud::Ptr corner(new PointCloud);
//   PointCloud::Ptr box(new PointCloud);
//   corner = GenerateCorner();
//   // Perturb a bit
//   PointCloud::Ptr corner_moved(new PointCloud);
//   Eigen::Matrix4f T = Eigen::Matrix4f::Identity();
//   T(0, 3) = 1;
//   T(1, 3) = -0.001;
//   pcl::transformPointCloudWithNormals(*corner, *corner_moved, T, true);

//   Eigen::Matrix4f T_est;
//   int inliers;
//   getTeaserInitialAlignment(corner, corner_moved, &T_est, inliers);

//   EXPECT_TRUE(T.isApprox(T_est));
//   EXPECT_EQ(300, inliers);
// }

TEST_F(TestLoopComputation, PerformAlignment) {
  ros::NodeHandle nh;
  icp_compute_.Initialize(nh);

  // Add some keyed scans
  PointCloud::Ptr corner(new PointCloud);
  PointCloud::Ptr box(new PointCloud);
  corner = GenerateCorner();
  // Perturb a bit
  PointCloud::Ptr corner_moved(new PointCloud);
  Eigen::Matrix4f T = Eigen::Matrix4f::Identity();
  T(0, 3) = 1;
  T(1, 3) = -0.001;
  pcl::transformPointCloudWithNormals(*corner, *corner_moved, T, true);

  pose_graph_msgs::KeyedScan::Ptr ks0(new pose_graph_msgs::KeyedScan);
  *ks0 = PointCloudToKeyedScan(corner, gtsam::Symbol('a', 0));
  pose_graph_msgs::KeyedScan::Ptr ks1(new pose_graph_msgs::KeyedScan);
  *ks1 = PointCloudToKeyedScan(corner_moved, gtsam::Symbol('a', 1));

  keyedScanCallback(ks0);
  keyedScanCallback(ks1);

  pose_graph_msgs::PoseGraph::Ptr kp(new pose_graph_msgs::PoseGraph);
  pose_graph_msgs::PoseGraphNode kp0, kp1;
  kp0.key = gtsam::Symbol('a', 0);
  kp1.key = gtsam::Symbol('a', 1);
  kp1.pose.position.x = 0.99;
  kp->nodes.push_back(kp0);
  kp->nodes.push_back(kp1);

  keyedPoseCallback(kp);

  geometry_utils::Transform3 tf, tf_exp;
  gtsam::Matrix66 covar;
  performAlignment(gtsam::Symbol('a', 1),
                   gtsam::Symbol('a', 0),
                   gtsam::Pose3(),
                   gtsam::Pose3(gtsam::Point3(0, 0, 0.99)),
                   &tf,
                   &covar);

  tf_exp.translation = geometry_utils::Vec3(T(0, 3), T(1, 3), T(2, 3));
  tf_exp.rotation = geometry_utils::Rot3(T(0, 0),
                                         T(0, 1),
                                         T(0, 2),
                                         T(1, 0),
                                         T(1, 1),
                                         T(1, 2),
                                         T(2, 0),
                                         T(2, 1),
                                         T(2, 2));

  EXPECT_EQ(tf_exp, tf);
}

}  // namespace lamp_loop_closure

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  ros::init(argc, argv, "test_loop_computation");
  return RUN_ALL_TESTS();
}

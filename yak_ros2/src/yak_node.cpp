#include <rclcpp/rclcpp.hpp>

#include <yak/yak_server.h>
#include <yak/mc/marching_cubes.h>

#include <tf2_ros/transform_listener.h>
#include <tf2_ros/buffer.h>
#include <tf2_eigen/tf2_eigen.h>

#include <cv_bridge/cv_bridge.h>

#include <sensor_msgs/msg/image.hpp>
#include <geometry_msgs/msg/transform_stamped.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <pcl/io/ply_io.h>

static const std::string DEFAULT_DEPTH_IMAGE_TOPIC = "input_depth_image";
static const std::double_t DEFAULT_MINIMUM_TRANSLATION = 0.00001;

/**
 * @brief The Fusion class. Integrate depth images into a TSDF volume. When requested, mesh the volume using marching
 * cubes. Note that this will work using both simulated and real robots and depth cameras.
 */
class Fusion
{
public:
  /**
   * @brief Fusion constructor
   * @param node - rclcpp node
   * @param params - KinFu parameters such as TSDF volume size, resolution, etc.
   * @param world_to_volume - Transform from world frame to volume origin frame.
   * @param tsdf_base_frame - The name of the tf frame that will be used as the base frame when performing lookups
   * between the TSDF volume and the camera frame.
   */
  explicit Fusion(const rclcpp::Node::SharedPtr node,
                  const kfusion::KinFuParams& params,
                  const Eigen::Affine3f& world_to_volume,
                  const std::string tsdf_base_frame)
    : node_(node)
    , clock_(std::make_shared<rclcpp::Clock>(RCL_SYSTEM_TIME))
    , tsdf_base_frame_(tsdf_base_frame)
    , tf_buffer_(clock_)
    , robot_tform_listener_(tf_buffer_)
    , fusion_(params, world_to_volume)
    , params_(params)
    , world_to_camera_prev_(Eigen::Affine3d::Identity())
  {
    // Subscribe to depth images published on the topic named by the depth_topic param. Set up callback to integrate
    // images when received.
    std::string depth_topic = DEFAULT_DEPTH_IMAGE_TOPIC;

    auto depth_image_cb = [this](const sensor_msgs::msg::Image::SharedPtr image_in) -> void {
      // Get the camera pose in the world frame at the time when the depth image was generated.
      RCLCPP_DEBUG(node_->get_logger(), "Got depth image");
      geometry_msgs::msg::TransformStamped transform_world_to_camera;
      try
      {
        transform_world_to_camera =
            tf_buffer_.lookupTransform(tsdf_base_frame_,
                                       image_in->header.frame_id,
                                       tf2::TimePoint(std::chrono::seconds(image_in->header.stamp.sec) +
                                                      std::chrono::nanoseconds(image_in->header.stamp.nanosec)),
                                       tf2::Duration(1000000000));
      }
      catch (tf2::TransformException& ex)
      {
        // Abort integration if tf lookup failed
        RCLCPP_WARN(node_->get_logger(), ex.what());
        return;
      }
      Eigen::Affine3d world_to_camera = tf2::transformToEigen(transform_world_to_camera);

      // Find how much the camera moved since the last depth image. If the magnitude of motion was below some threshold,
      // abort integration. This is to prevent noise from accumulating in the isosurface due to numerous observations
      // from the same pose.
      std::double_t motion_mag = (world_to_camera.inverse() * world_to_camera_prev_).translation().norm();

      if (motion_mag < DEFAULT_MINIMUM_TRANSLATION)
      {
        RCLCPP_DEBUG(node_->get_logger(), "Camera motion below threshold");
        return;
      }

      cv_bridge::CvImagePtr cv_ptr = cv_bridge::toCvCopy(image_in, sensor_msgs::image_encodings::TYPE_16UC1);

      // Integrate the depth image into the TSDF volume
      if (!fusion_.fuse(cv_ptr->image, world_to_camera.cast<float>()))
      {
        RCLCPP_WARN(node_->get_logger(), "Failed to fuse image");
      }

      // If integration was successful, update the previous camera pose with the new camera pose
      world_to_camera_prev_ = world_to_camera;
      return;
    };

    depth_image_sub_ = node_->create_subscription<sensor_msgs::msg::Image>(depth_topic, 10, depth_image_cb);

    auto generate_mesh_cb = [this](const std::shared_ptr<rmw_request_id_t> request_header,
                                   const std::shared_ptr<std_srvs::srv::Trigger::Request> req,
                                   const std::shared_ptr<std_srvs::srv::Trigger::Response> res) -> void {
      RCLCPP_INFO(node_->get_logger(), "Starting mesh generation");
      yak::MarchingCubesParameters mc_params;
      mc_params.scale = params_.volume_resolution;
      pcl::PolygonMesh mesh = yak::marchingCubesCPU(fusion_.downloadTSDF(), mc_params);
      RCLCPP_INFO(node_->get_logger(), "Meshing done, saving ply");
      pcl::io::savePLYFileBinary("cubes.ply", mesh);
      RCLCPP_INFO(node_->get_logger(), "Saving done");
      res->success = true;
    };

    // Advertise service for marching cubes meshing
    generate_mesh_service_ = node_->create_service<std_srvs::srv::Trigger>("generate_mesh_service", generate_mesh_cb);
  }

private:
  rclcpp::Node::SharedPtr node_;
  rclcpp::Clock::SharedPtr clock_;
  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr depth_image_sub_;
  rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr generate_mesh_service_;

  std::string tsdf_base_frame_;
  tf2_ros::Buffer tf_buffer_;
  tf2_ros::TransformListener robot_tform_listener_;

  yak::FusionServer fusion_;
  const kfusion::KinFuParams params_;
  Eigen::Affine3d world_to_camera_prev_;

public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
};

int main(int argc, char* argv[])
{
  rclcpp::init(argc, argv);
  auto node = rclcpp::Node::make_shared("yak_node");

  node->declare_parameter("tsdf_frame_id");
  node->declare_parameter("use_pose_hints");
  node->declare_parameter("use_icp");
  node->declare_parameter("update_via_sensor_motion");
  node->declare_parameter("camera_intrinsic_params.fx");
  node->declare_parameter("camera_intrinsic_params.fy");
  node->declare_parameter("camera_intrinsic_params.cx");
  node->declare_parameter("camera_intrinsic_params.cy");
  node->declare_parameter("cols");
  node->declare_parameter("rows");
  node->declare_parameter("volume_x");
  node->declare_parameter("volume_y");
  node->declare_parameter("volume_z");
  node->declare_parameter("voxel_resolution");

  std::string tsdf_base_frame;
  node->get_parameter_or<std::string>("tsdf_frame_id", tsdf_base_frame, "tsdf_origin");

  // Set up TSDF parameters
  kfusion::KinFuParams params = kfusion::KinFuParams::default_params();

  node->get_parameter_or("use_pose_hints",
                         params.use_pose_hints,
                         true);  // use robot forward kinematics to find camera pose relative to TSDF volume
  node->get_parameter_or("use_icp",
                         params.use_icp,
                         false);  // since we're using robot FK to get the camera pose, don't use ICP (TODO: yet!)
  node->get_parameter_or("update_via_sensor_motion", params.update_via_sensor_motion, false);  // deprecated?

  node->get_parameter_or("camera_intrinsic_params.fx", params.intr.fx, 550.0f);
  node->get_parameter_or("camera_intrinsic_params.fy", params.intr.fy, 550.0f);
  node->get_parameter_or("camera_intrinsic_params.cx", params.intr.cx, 320.0f);
  node->get_parameter_or("camera_intrinsic_params.cy", params.intr.cy, 240.0f);

  node->get_parameter_or("cols", params.cols, 640);
  node->get_parameter_or("rows", params.rows, 480);

  int voxels_x, voxels_y, voxels_z;
  node->get_parameter_or("volume_x", voxels_x, 640);
  node->get_parameter_or("volume_y", voxels_y, 640);
  node->get_parameter_or("volume_z", voxels_z, 192);

  node->get_parameter_or<float>("voxel_resolution", params.volume_resolution, 0.001f);

  // TODO: Autocompute resolution from volume length/width/height in meters
  params.volume_dims = cv::Vec3i(voxels_x, voxels_y, voxels_z);
  params.volume_pose = Eigen::Affine3f::Identity();          // This gets overwritten when Yak is initialized
  params.tsdf_trunc_dist = params.volume_resolution * 5.0f;  // meters;
  params.tsdf_max_weight = 50;                               // frames
  params.raycast_step_factor = 0.25;                         // in voxel sizes
  params.gradient_delta_factor = 0.25;                       // in voxel sizes

  RCLCPP_INFO(node->get_logger(), "Starting fusion node");

  auto fusion = std::make_shared<Fusion>(node, params, Eigen::Affine3f::Identity(), tsdf_base_frame);

  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}

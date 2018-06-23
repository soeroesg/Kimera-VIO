/* ----------------------------------------------------------------------------
 * Copyright 2017, Massachusetts Institute of Technology,
 * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Luca Carlone, et al. (see THANKS for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

/**
 * @file   Mesher.h
 * @brief  Build and visualize 2D mesh from Frame
 * @author Luca Carlone, AJ Haeffner, Antoni Rosinol
 */

#include "mesh/Mesher.h"

#include <algorithm>

#include "LoggerMatlab.h"
#include <opencv2/imgproc.hpp>

#include <glog/logging.h>
#include <gflags/gflags.h>
// General functionality for the mesher.
DEFINE_bool(add_extra_lmks_from_stereo, false,
            "Add extra landmarks that are stereo triangulated to the mesh. "
            "WARNING this is computationally expensive.");
DEFINE_bool(reduce_mesh_to_time_horizon, true, "Reduce mesh vertices to the "
            "landmarks available in current optimization's time horizon.");

// Visualization.
DEFINE_bool(visualize_histogram_1D, false, "Visualize 1D histogram.");
DEFINE_bool(visualize_histogram_2D, false, "Visualize 2D histogram.");
DEFINE_bool(visualize_mesh_2d, false, "Visualize mesh 2D.");
DEFINE_bool(visualize_mesh_2d_filtered, false, "Visualize mesh 2D filtered.");

// Mesh filters.
DEFINE_double(max_grad_in_triangle, -1,
              "Maximum allowed gradient inside a triangle.");
DEFINE_double(min_ratio_btw_largest_smallest_side, 0.5,
              "Minimum ratio between largest and smallest "
              "side of a triangle."); // TODO: this check should be improved
DEFINE_double(min_elongation_ratio, 0.5, "Minimum allowed elongation "
                                         "ratio for a triangle.");  // TODO: this check should be improved
DEFINE_double(max_triangle_side, 0.5, "Maximum allowed side for "
                                      "a triangle.");

// Association.
DEFINE_double(normal_tolerance_polygon_plane_association, 0.011,
              "Tolerance for a polygon's normal and a plane's normal to be "
              "considered equal (0.087 === 10 deg. aperture).");
DEFINE_double(distance_tolerance_polygon_plane_association, 0.10,
              "Tolerance for a polygon vertices to be considered close to a "
              "plane.");
DEFINE_double(normal_tolerance_plane_plane_association, 0.011,
              "Normal tolerance for a plane to be associated to another plane "
              "(0.087 === 10 deg. aperture).");
DEFINE_double(distance_tolerance_plane_plane_association, 0.20,
              "Distance tolerance for a plane to be associated to another "
              "plane.");
DEFINE_bool(do_double_association, true,
            "Do double plane association of backend plane with multiple "
            "segmented planes. Otherwise search for another possible "
            "backend plane for the segmented plane.");

// Segmentation.
DEFINE_double(normal_tolerance_horizontal_surface, 0.011,
              "Normal tolerance for a polygon to be considered parallel to the "
              "ground (0.087 === 10 deg. aperture).");
DEFINE_double(normal_tolerance_walls, 0.0165,
              "Normal tolerance for a polygon to be considered perpendicular to"
              " the vertical direction.");
DEFINE_bool(only_use_non_clustered_points, true,
            "Only use points that have not been clustered in a plane already "
            "when filling both histograms.");

// Histogram 2D.
DEFINE_int32(hist_2d_gaussian_kernel_size, 3,
             "Kernel size for gaussian blur of 2D histogram.");
DEFINE_int32(hist_2d_nr_of_local_max, 2,
             "Number of local maximums to extract in 2D histogram.");
DEFINE_int32(hist_2d_min_support, 20,
             "Minimum number of votes to consider a local maximum in 2D "
             "histogram a valid peak.");
DEFINE_int32(hist_2d_min_dist_btw_local_max, 5,
             "Minimum distance between local maximums to be considered different.");
DEFINE_int32(hist_2d_theta_bins, 40, ".");
DEFINE_int32(hist_2d_distance_bins, 40, ".");
DEFINE_double(hist_2d_theta_range_min, 0, ".");
DEFINE_double(hist_2d_theta_range_max, PI, ".");
DEFINE_double(hist_2d_distance_range_min, -6.0, ".");
DEFINE_double(hist_2d_distance_range_max, 6.0, ".");

// Z histogram.
DEFINE_int32(z_histogram_bins, 512, "Number of bins for z histogram.");
DEFINE_double(z_histogram_min_range, -0.75, "Minimum z value for z histogram.");
DEFINE_double(z_histogram_max_range, 3.0, "Maximum z value for z histogram.");
DEFINE_int32(z_histogram_window_size, 3, "Window size of z histogram to "
             "calculate derivatives, not sure in fact.");
DEFINE_double(z_histogram_peak_per, 0.5,
              "Extra peaks in the z histogram will be only considered if it "
              "has a value of peak_per (< 1) times the value of the max peak"
              " in the histogram.");
DEFINE_double(z_histogram_min_support, 50,
              "Minimum number of votes for a value in the z histogram to be "
              "considered a peak.");
DEFINE_double(z_histogram_min_separation, 0.1,
             "If two peaks in the z histogram lie within min_separation "
             ", only the one with maximum support will be taken "
             "(sisable by setting < 0).");
DEFINE_int32(z_histogram_gaussian_kernel_size, 5,
             "Kernel size for gaussian blur of z histogram (should be odd).");
DEFINE_int32(z_histogram_max_number_of_peaks_to_select, 3,
             "Maximum number of peaks to select in z histogram.");

namespace VIO {

/* -------------------------------------------------------------------------- */
Mesher::Mesher()
  : mesh_() {
  // Create z histogram.
  std::vector<int> hist_size = {FLAGS_z_histogram_bins};
  std::array<float, 2> z_range = {FLAGS_z_histogram_min_range,
                                  FLAGS_z_histogram_max_range};
  std::vector<std::array<float, 2>> ranges = {z_range};
  std::vector<int> channels = {0};
  z_hist_ = Histogram(1, channels, cv::Mat(), 1, hist_size, ranges, true, false);

  // Create 2d histogram.
  std::vector<int> hist_2d_size = {FLAGS_hist_2d_theta_bins,
                                   FLAGS_hist_2d_distance_bins};
  std::array<float, 2> theta_range = {FLAGS_hist_2d_theta_range_min,
                                      FLAGS_hist_2d_theta_range_max};
  std::array<float, 2> distance_range = {FLAGS_hist_2d_distance_range_min,
                                         FLAGS_hist_2d_distance_range_max};
  std::vector<std::array<float, 2>> ranges_2d = {theta_range, distance_range};
  std::vector<int> channels_2d = {0, 1};
  hist_2d_ = Histogram(1, channels_2d, cv::Mat(), 2, hist_2d_size, ranges_2d,
                       true, false);
}


/* -------------------------------------------------------------------------- */
// For a triangle defined by the 3d points p1, p2, and p3
// compute ratio between largest side and smallest side (how elongated it is).
double Mesher::getRatioBetweenSmallestAndLargestSide(
    const double& d12,
    const double& d23,
    const double& d31,
    boost::optional<double &> minSide_out,
    boost::optional<double &> maxSide_out) const {

  // Measure sides.
  double minSide = std::min(d12, std::min(d23, d31));
  double maxSide = std::max(d12, std::max(d23, d31));

  if(minSide_out && maxSide_out){
    *minSide_out = minSide;
    *maxSide_out = maxSide;
  }

  // Compute and return ratio.
  return minSide / maxSide;
}

/* -------------------------------------------------------------------------- */
// TODO this only works for current points in the current frame!!!
// Not for the landmarks in time horizon, since they can be behind the camera!!!
// for a triangle defined by the 3d points mapPoints3d_.at(rowId_pt1), mapPoints3d_.at(rowId_pt2),
// mapPoints3d_.at(rowId_pt3), compute ratio between largest side and smallest side (how elongated it is)
double Mesher::getRatioBetweenTangentialAndRadialDisplacement(
    const Mesh3D::VertexPosition3D& p1,
    const Mesh3D::VertexPosition3D& p2,
    const Mesh3D::VertexPosition3D& p3,
    const gtsam::Pose3& leftCameraPose) const {
  std::vector<gtsam::Point3> points;

  // get 3D points
  gtsam::Point3 p1_C = gtsam::Point3(double(p1.x),
                                     double(p1.y),
                                     double(p1.z));
  points.push_back(leftCameraPose.transform_to(p1_C)); // checks elongation in *camera frame*

  gtsam::Point3 p2_C = gtsam::Point3(double(p2.x),
                                     double(p2.y),
                                     double(p2.z));
  points.push_back(leftCameraPose.transform_to(p2_C)); // checks elongation in *camera frame*

  gtsam::Point3 p3_C = gtsam::Point3(double(p3.x),
                                     double(p3.y),
                                     double(p3.z));
  points.push_back(leftCameraPose.transform_to(p3_C)); // checks elongation in *camera frame*

  return UtilsGeometry::getRatioBetweenTangentialAndRadialDisplacement(points);
}

/* -------------------------------------------------------------------------- */
// Try to reject bad triangles, corresponding to outliers
// TODO filter out bad triangle without s, and use it in reduce Mesh.
// TODO filter before and not using the mesh itself because there are lmks
// that might not be seen in the current frame!
void Mesher::filterOutBadTriangles(const gtsam::Pose3& leftCameraPose,
                                   double minRatioBetweenLargestAnSmallestSide,
                                   double min_elongation_ratio,
                                   double maxTriangleSide) {
  Mesh3D mesh_output;

  // Loop over each face in the mesh.
  Mesh3D::Polygon polygon;

  for (size_t i = 0; i < mesh_.getNumberOfPolygons(); i++) {
    CHECK(mesh_.getPolygon(i, &polygon)) << "Could not retrieve polygon.";
    CHECK_EQ(polygon.size(), 3) << "Expecting 3 vertices in triangle";
   // Check if triangle is good.
    if (!isBadTriangle(polygon, leftCameraPose,
                       minRatioBetweenLargestAnSmallestSide,
                       min_elongation_ratio,
                       maxTriangleSide)) {
      mesh_output.addPolygonToMesh(polygon);
    }
  }

  mesh_ = mesh_output;
}

/* -------------------------------------------------------------------------- */
// Try to reject bad triangles, corresponding to outliers.
bool Mesher::isBadTriangle(
                       const Mesh3D::Polygon& polygon,
                       const gtsam::Pose3& left_camera_pose,
                       const double& min_ratio_between_largest_an_smallest_side,
                       const double& min_elongation_ratio,
                       const double& max_triangle_side) const {
    CHECK_EQ(polygon.size(), 3) << "Expecting 3 vertices in triangle";
    const Mesh3D::VertexPosition3D& p1 = polygon.at(0).getVertexPosition();
    const Mesh3D::VertexPosition3D& p2 = polygon.at(1).getVertexPosition();
    const Mesh3D::VertexPosition3D& p3 = polygon.at(2).getVertexPosition();

    double ratioSides_i = 0;
    double ratioTangentialRadial_i = 0;
    double maxTriangleSide_i = 0;

    // Check geometric dimensions.
    // Measure sides.
    double d12 = cv::norm(p1 - p2);
    double d23 = cv::norm(p2 - p3);
    double d31 = cv::norm(p3 - p1);

    // If threshold is disabled, avoid computation.
    if (min_ratio_between_largest_an_smallest_side > 0.0) {
      ratioSides_i = getRatioBetweenSmallestAndLargestSide(
                      d12, d23, d31);
    }

    // If threshold is disabled, avoid computation.
    if (min_elongation_ratio > 0.0) {
      ratioTangentialRadial_i = getRatioBetweenTangentialAndRadialDisplacement(
                                  p1, p2, p3,
                                  left_camera_pose);
    }

    // If threshold is disabled, avoid computation.
    if (max_triangle_side > 0.0) {
      std::array<double, 3> sidesLen;
      sidesLen.at(0) = d12;
      sidesLen.at(1) = d23;
      sidesLen.at(2) = d31;
      const auto& it = std::max_element(sidesLen.begin(), sidesLen.end());
      DCHECK(it != sidesLen.end());
      maxTriangleSide_i = *it;
    }

    // Check if triangle is not elongated.
    if ((ratioSides_i >= min_ratio_between_largest_an_smallest_side) &&
        (ratioTangentialRadial_i >= min_elongation_ratio) &&
        (maxTriangleSide_i <= max_triangle_side)) {
      return false;
    } else {
      return true;
    }
}

/* -------------------------------------------------------------------------- */
// Create a 3D mesh from 2D corners in an image, keeps the mesh in time horizon.
void Mesher::populate3dMeshTimeHorizon(
    const std::vector<cv::Vec6f>& mesh_2d, // cv::Vec6f assumes triangular mesh.
    const std::unordered_map<LandmarkId, gtsam::Point3>& points_with_id_map,
    const Frame& frame,
    const gtsam::Pose3& leftCameraPose,
    double min_ratio_largest_smallest_side,
    double min_elongation_ratio,
    double max_triangle_side) {
  VLOG(10) << "Starting populate3dMeshTimeHorizon...";
  VLOG(10) << "Starting populate3dMesh...";
  populate3dMesh(mesh_2d, points_with_id_map, frame, leftCameraPose,
                 min_ratio_largest_smallest_side,
                 min_elongation_ratio,
                 max_triangle_side);
  VLOG(10) << "Finished populate3dMesh.";

  // Remove faces in the mesh that have vertices which are not in
  // points_with_id_map anymore.
  VLOG(10) << "Starting updatePolygonMeshToTimeHorizon...";
  updatePolygonMeshToTimeHorizon(points_with_id_map,
                                 leftCameraPose,
                                 min_ratio_largest_smallest_side,
                                 max_triangle_side,
                                 FLAGS_reduce_mesh_to_time_horizon);
  VLOG(10) << "Finished updatePolygonMeshToTimeHorizon.";
  VLOG(10) << "Finished populate3dMeshTimeHorizon.";
}

/* -------------------------------------------------------------------------- */
// Create a 3D mesh from 2D corners in an image.
void Mesher::populate3dMesh(
    const std::vector<cv::Vec6f>& mesh_2d, // cv::Vec6f assumes triangular mesh.
    const std::unordered_map<LandmarkId, gtsam::Point3>& points_with_id_map,
    const Frame& frame,
    const gtsam::Pose3& leftCameraPose,
    double min_ratio_largest_smallest_side,
    double min_elongation_ratio,
    double max_triangle_side) {
  // Iterate over each face in the 2d mesh, and generate the 3d mesh.
  // TODO to retrieve lmk id from pixels, do it in the stereo frame! not here.
  // Create polygon and add it to the mesh.
  Mesh3D::Polygon polygon;
  polygon.resize(3);
  // Iterate over the 2d mesh triangles.
  for (size_t i = 0; i < mesh_2d.size(); i++) {
    const cv::Vec6f& triangle_2d = mesh_2d.at(i);

    // Iterate over each vertex (pixel) of the triangle.
    // Triangle_2d.rows = 3.
    for (size_t j = 0; j < triangle_2d.rows / 2; j++) {
      // Extract pixel.
      const cv::Point2f pixel (triangle_2d[j * 2],
                               triangle_2d[j * 2 + 1]);

      // Extract landmark id corresponding to this pixel.
      const LandmarkId lmk_id (frame.findLmkIdFromPixel(pixel));
      CHECK_NE(lmk_id, -1);

      // Try to find this landmark id in points_with_id_map.
      const auto& lmk_it = points_with_id_map.find(lmk_id);
      if (lmk_it != points_with_id_map.end()) {
        // We found the landmark.
        // Extract 3D position of the landmark.
        const gtsam::Point3& point (lmk_it->second);
        cv::Point3f lmk(float(point.x()),
                        float(point.y()),
                        float(point.z()));
        // Add landmark as one of the vertices of the current polygon in 3D.
        DCHECK_LT(j, polygon.size());
        polygon.at(j) = Mesh3D::Vertex(lmk_id, lmk);
        static const size_t loop_end = triangle_2d.rows / 2 - 1;
        if (j == loop_end) {
          // Last iteration.
          // Filter out bad polygons.
          if (!isBadTriangle(polygon, leftCameraPose,
                             min_ratio_largest_smallest_side,
                             min_elongation_ratio,
                             max_triangle_side)) {
            // Save the valid triangular polygon, since it has all vertices in
            // points_with_id_map.
            mesh_.addPolygonToMesh(polygon);
          }
        }
      } else {
        // Do not save current polygon, since it has at least one vertex that
        // is not in points_with_id_map.
        LOG(ERROR) << "Landmark with id : " << lmk_id
                   << ", could not be found in points_with_id_map. "
                   << "But it should have been.\n";
        break;
      }
    }
  }
}

/* -------------------------------------------------------------------------- */
// TODO the polygon_mesh has repeated faces...
// And this seems to slow down quite a bit the for loop!
void Mesher::updatePolygonMeshToTimeHorizon(
    const std::unordered_map<LandmarkId, gtsam::Point3>& points_with_id_map,
    const gtsam::Pose3& leftCameraPose,
    double min_ratio_largest_smallest_side,
    double max_triangle_side,
    const bool& reduce_mesh_to_time_horizon) {
  VLOG(10) << "Starting updatePolygonMeshToTimeHorizon...";
  Mesh3D mesh_output;

  auto end = points_with_id_map.end();
  // Loop over each face in the mesh.
  Mesh3D::Polygon polygon;

  for (size_t i = 0; i < mesh_.getNumberOfPolygons(); i++) {
    CHECK(mesh_.getPolygon(i, &polygon)) << "Could not retrieve polygon.";
    bool save_polygon = true;
    for (Mesh3D::Vertex& vertex: polygon) {
      const auto& point_with_id_it = points_with_id_map.find(vertex.getLmkId());
      if (point_with_id_it == end) {
        // Vertex of current polygon is not in points_with_id_map
        if (reduce_mesh_to_time_horizon) {
          // We want to reduce the mesh to time horizon.
          // Delete the polygon by not adding it to the new mesh.
          save_polygon = false;
          break;
        } else {
          // We do not want to reduce the mesh to time horizon.
          save_polygon = true;
        }
      } else {
        // Update the vertex with newest landmark position.
        // This is to ensure we have latest update, the previous addPolygonToMesh
        // only updates the positions of the vertices in the visible frame.
        vertex.setVertexPosition(Mesh3D::VertexPosition3D(
                                   point_with_id_it->second.x(),
                                   point_with_id_it->second.y(),
                                   point_with_id_it->second.z()));
      }
    }

    if (save_polygon) {
      // Refilter polygons, as the updated vertices might make it unvalid.
      if (!isBadTriangle(polygon,
                         leftCameraPose,
                         min_ratio_largest_smallest_side,
                         -1.0, // elongation test is invalid, no per-frame concept
                         max_triangle_side)) {
        // Finally add the polygon to the mesh.
        mesh_output.addPolygonToMesh(polygon);
      }
    }
  }

  mesh_ = mesh_output;
  VLOG(10) << "Finished updatePolygonMeshToTimeHorizon.";
}

/* -------------------------------------------------------------------------- */
// Calculate normals of polygonMesh.
void Mesher::calculateNormals(std::vector<cv::Point3f>* normals) {
  CHECK_NOTNULL(normals);
  CHECK_EQ(mesh_.getMeshPolygonDimension(), 3)
      << "Expecting 3 vertices in triangle.";

  // Brute force, ideally only call when a new triangle appears...
  normals->clear();
  normals->resize(mesh_.getNumberOfPolygons()); // TODO Assumes we have triangles...

  // Loop over each polygon face in the mesh.
  // TODO there are far too many loops over the total number of Polygon faces...
  // Should put them all in the same loop!
  Mesh3D::Polygon polygon;
  for (size_t i = 0; i < mesh_.getNumberOfPolygons(); i++) {
    CHECK(mesh_.getPolygon(i, &polygon)) << "Could not retrieve polygon.";
    CHECK_EQ(polygon.size(), 3);
    const Mesh3D::VertexPosition3D& p1 = polygon.at(0).getVertexPosition();
    const Mesh3D::VertexPosition3D& p2 = polygon.at(1).getVertexPosition();
    const Mesh3D::VertexPosition3D& p3 = polygon.at(2).getVertexPosition();

    cv::Point3f normal;
    CHECK(calculateNormal(p1, p2, p3, &normal));
    // Mat normal2;
    // viz::computeNormals(mesh, normal2);
    // https://github.com/zhoushiwei/Viz-opencv/blob/master/Viz/main.cpp

    // Store normal to triangle i.
    normals->at(i) = normal;
  }
}

/* -------------------------------------------------------------------------- */
// Calculate normal of a triangle, and return whether it was possible or not.
// Calculating the normal of aligned points in 3D is not possible...
bool Mesher::calculateNormal(const Mesh3D::VertexPosition3D& p1,
                             const Mesh3D::VertexPosition3D& p2,
                             const Mesh3D::VertexPosition3D& p3,
                             cv::Point3f* normal) const {
  CHECK_NOTNULL(normal);
  // TODO what if p2 = p1 or p3 = p1?
  // Calculate vectors of the triangle.
  cv::Point3f v21 = p2 - p1;
  cv::Point3f v31 = p3 - p1;

  // Normalize vectors.
  double v21_norm = cv::norm(v21);
  CHECK_GT(v21_norm, 0.0);
  v21 /= v21_norm;

  double v31_norm = cv::norm(v31);
  CHECK_GT(v31_norm, 0.0);
  v31 /= v31_norm;

  // Check that vectors are not aligned, dot product should not be 1 or -1.
  static constexpr double epsilon = 1e-3; // 2.5 degrees aperture.
  if (std::fabs(v21.ddot(v31)) >= 1.0 - epsilon) {
    // Dot prod very close to 1.0 or -1.0...
    // We have a degenerate configuration with aligned vectors.
    LOG(WARNING) << "Cross product of aligned vectors.";
    return false;
  } else {
    // Calculate normal (cross product).
    *normal = v21.cross(v31);

    // Normalize.
    double norm = cv::norm(*normal);
    CHECK_GT(norm, 0.0);
    *normal /= norm;
    CHECK_NEAR(cv::norm(*normal), 1.0, 1e-5); // Expect unit norm.
    return true;
  }
}

/* -------------------------------------------------------------------------- */
// Clusters normals given an axis, a set of normals and a
// tolerance. The result is a vector of indices of the given set of normals
// that are in the cluster.
void Mesher::clusterNormalsAroundAxis(const cv::Point3f& axis,
                                      const std::vector<cv::Point3f>& normals,
                                      const double& tolerance,
                                      std::vector<int>* cluster_normals_idx) {
  size_t idx = 0;
  // TODO, this should be in the same loop as the one calculating
  // the normals...
  for (const cv::Point3f& normal: normals) {
    if (isNormalAroundAxis(axis, normal, tolerance))
      cluster_normals_idx->push_back(idx);
    idx++;
  }
}

/* -------------------------------------------------------------------------- */
// Is normal around axis?
bool Mesher::isNormalAroundAxis(const cv::Point3f& axis,
                                const cv::Point3f& normal,
                                const double& tolerance) const {
  // TODO typedef normals and axis to Normal, and use cv::Point3d instead.
  CHECK_NEAR(cv::norm(axis), 1.0, 1e-5); // Expect unit norm.
  CHECK_NEAR(cv::norm(normal), 1.0, 1e-5); // Expect unit norm.
  CHECK_GT(tolerance, 0.0); // Tolerance is positive.
  CHECK_LT(tolerance, 1.0); // Tolerance is lower than maximum dot product.
  // Dot product should be close to 1 or -1 if axis is aligned with normal.
  return (std::fabs(normal.ddot(axis)) > 1.0 - tolerance);
}

/* -------------------------------------------------------------------------- */
// Clusters normals perpendicular to an axis. Given an axis, a set of normals and a
// tolerance. The result is a vector of indices of the given set of normals
// that are in the cluster.
void Mesher::clusterNormalsPerpendicularToAxis(const cv::Point3f& axis,
                                      const std::vector<cv::Point3f>& normals,
                                      const double& tolerance,
                                      std::vector<int>* cluster_normals_idx) {
  size_t idx = 0;
  // TODO, this should be in the same loop as the one calculating
  // the normals...
  // TODO: remove logger.
  static constexpr bool log_normals = false;
  std::vector<cv::Point3f> cluster_normals;
  for (const cv::Point3f& normal: normals) {
    if (isNormalPerpendicularToAxis(axis, normal, tolerance)) {
      cluster_normals_idx->push_back(idx);
      // TODO: remove logger.
      if (log_normals) {
        cluster_normals.push_back(normal);
      }
    }
    idx++;
  }
  if (log_normals) {
    LoggerMatlab logger;
    logger.openLogFiles(4);
    logger.logNormals(cluster_normals);
    logger.closeLogFiles(4);
  }
}

/* -------------------------------------------------------------------------- */
// Is normal perpendicular to axis?
bool Mesher::isNormalPerpendicularToAxis(const cv::Point3f& axis,
                                         const cv::Point3f& normal,
                                         const double& tolerance) const {
  CHECK_NEAR(cv::norm(axis), 1.0, 1e-5); // Expect unit norm.
  CHECK_NEAR(cv::norm(normal), 1.0, 1e-5); // Expect unit norm.
  CHECK_GT(tolerance, 0.0); // Tolerance is positive.
  CHECK_LT(tolerance, 1.0); // Tolerance is lower than maximum dot product.
  // Dot product should be close to 0 if axis is perpendicular to normal.
  return (cv::norm(normal.ddot(axis)) < tolerance);
}

/* -------------------------------------------------------------------------- */
// Checks whether all points in polygon are closer than tolerance to the plane.
bool Mesher::isPolygonAtDistanceFromPlane(const Mesh3D::Polygon& polygon,
                                          const double& plane_distance,
                                          const cv::Point3f& plane_normal,
                                          const double& distance_tolerance)
const {
  CHECK_NEAR(cv::norm(plane_normal), 1.0, 1e-05); // Expect unit norm.
  CHECK_GE(distance_tolerance, 0.0);
  for (const Mesh3D::Vertex& vertex: polygon) {
    if (!isPointAtDistanceFromPlane(vertex.getVertexPosition(),
                                    plane_distance, plane_normal,
                                    distance_tolerance)) {
      return false;
    }
  }
  // All lmks are close to the plane.
  return true;
}

/* -------------------------------------------------------------------------- */
// Checks whether the point is closer than tolerance to the plane.
bool Mesher::isPointAtDistanceFromPlane(
    const Mesh3D::VertexPosition3D& point,
    const double& plane_distance,
    const cv::Point3f& plane_normal,
    const double& distance_tolerance) const {
  CHECK_NEAR(cv::norm(plane_normal), 1.0, 1e-05); // Expect unit norm.
  CHECK_GE(distance_tolerance, 0.0);
  // The lmk is closer to the plane than given tolerance.
  return (std::fabs(plane_distance - point.ddot(plane_normal)) <=
          distance_tolerance);
}

/* -------------------------------------------------------------------------- */
// Cluster planes from Mesh.
// Points_with_id_vio are only used when add_extra_lmks_from_stereo is true, so
// that we only extract lmk ids that are in the optimization time horizon.
void Mesher::clusterPlanesFromMesh(
    std::vector<Plane>* planes,
    const std::unordered_map<LandmarkId, gtsam::Point3>& points_with_id_vio) {
  CHECK_NOTNULL(planes);
  // Segment planes in the mesh, using seeds.
  VLOG(10) << "Starting plane segmentation...";
  std::vector<Plane> new_planes;
  segmentPlanesInMesh(planes, &new_planes,
                      points_with_id_vio,
                      FLAGS_normal_tolerance_polygon_plane_association,
                      FLAGS_distance_tolerance_polygon_plane_association,
                      FLAGS_normal_tolerance_horizontal_surface,
                      FLAGS_normal_tolerance_walls);
  VLOG(10) << "Finished plane segmentation.";
  // Do data association between the planes given and the ones segmented.
  VLOG(10) << "Starting plane association...";
  std::vector<Plane> new_non_associated_planes;
  associatePlanes(new_planes, *planes, &new_non_associated_planes,
                  FLAGS_normal_tolerance_plane_plane_association,
                  FLAGS_distance_tolerance_plane_plane_association);
  VLOG(10) << "Finished plane association.";
  if (new_non_associated_planes.size() > 0) {
    // Update lmk ids of the newly added planes.

    // TODO delete this loop by customizing histograms!!
    // WARNING Here we are updating lmk ids in new non-associated planes,
    // BUT it requires another loop over mesh, and recalculates normals!!!
    // Very unefficient.
    VLOG(10) << "Starting update plane lmk ids for new non-associated planes.";
    updatePlanesLmkIdsFromMesh(&new_non_associated_planes,
                               FLAGS_normal_tolerance_polygon_plane_association,
                               FLAGS_distance_tolerance_polygon_plane_association,
                               points_with_id_vio);
    VLOG(10) << "Finished update plane lmk ids for new non-associated planes.";

    // Append new planes that where not associated to original planes.
    planes->insert(planes->end(),
                   new_non_associated_planes.begin(),
                   new_non_associated_planes.end());
  } else {
    VLOG(10) << "Avoid extra loop over mesh, since there are no new non-associated"
               " planes to be updated.";
  }
}

/* -------------------------------------------------------------------------- */
// Segment planes in the mesh: updates seed_planes and extracts new_planes.
// Points_with_id_vio are only used if add_extra_lmks_from_stereo is true,
// They are used by extractLmkIdsFromTriangleCluster to extract only lmk ids
// that are in the time horizon (aka points_with_id_vio).
void Mesher::segmentPlanesInMesh(
    std::vector<Plane>* seed_planes,
    std::vector<Plane>* new_planes,
    const std::unordered_map<LandmarkId, gtsam::Point3>& points_with_id_vio,
    const double& normal_tolerance_polygon_plane_association,
    const double& distance_tolerance_polygon_plane_association,
    const double& normal_tolerance_horizontal_surface,
    const double& normal_tolerance_walls) {
  CHECK_NOTNULL(seed_planes);
  CHECK_NOTNULL(new_planes);

  // Clean seed_planes of lmk_ids:
  for (Plane& seed_plane: *seed_planes) {
    seed_plane.lmk_ids_.clear();
    seed_plane.triangle_cluster_.triangle_ids_.clear();
  }

  static constexpr size_t mesh_polygon_dim = 3;
  CHECK_EQ(mesh_.getMeshPolygonDimension(), mesh_polygon_dim)
      << "Expecting 3 vertices in triangle.";

  // Cluster new lmk ids for seed planes.
  // Loop over the mesh only once.
  Mesh3D::Polygon polygon;
  cv::Mat z_components (1, 0, CV_32F);
  cv::Mat walls (0, 0, CV_32FC2);
  for (size_t i = 0; i < mesh_.getNumberOfPolygons(); i++) {
    CHECK(mesh_.getPolygon(i, &polygon)) << "Could not retrieve polygon.";
    CHECK_EQ(polygon.size(), mesh_polygon_dim);
    const Mesh3D::VertexPosition3D& p1 = polygon.at(0).getVertexPosition();
    const Mesh3D::VertexPosition3D& p2 = polygon.at(1).getVertexPosition();
    const Mesh3D::VertexPosition3D& p3 = polygon.at(2).getVertexPosition();

    // Calculate normal of the triangle in the mesh.
    // The normals are in the world frame of reference.
    cv::Point3f triangle_normal;
    if (calculateNormal(p1, p2, p3, &triangle_normal)) {
      ////////////////////////// Update seed planes ////////////////////////////
      // Update seed_planes lmk_ids field with ids of vertices of polygon if the
      // polygon is on the plane.
      bool is_polygon_on_a_plane = updatePlanesLmkIdsFromPolygon(
                                     seed_planes, polygon,
                                     i, triangle_normal,
                                     normal_tolerance_polygon_plane_association,
                                     distance_tolerance_polygon_plane_association,
                                     points_with_id_vio);

      ////////////////// Build Histogram for new planes ////////////////////////
      /// Values for Z Histogram.///////////////////////////////////////////////
      // Collect z values of vertices of polygon which is not already on a plane
      // and which has the normal aligned with the vertical direction so that we
      // can build an histogram.
      static const cv::Point3f vertical (0, 0, 1);
      if ((FLAGS_only_use_non_clustered_points?
           !is_polygon_on_a_plane : true) &&
          isNormalAroundAxis(vertical, triangle_normal, normal_tolerance_horizontal_surface)) {
        // We have a triangle with a normal aligned with gravity, which is not
        // already clustered in a plane.
        // Store z components to build histogram.
        // TODO instead of storing z_components, use the accumulate flag in
        // calcHist and add them straight.
        z_components.push_back(p1.z);
        z_components.push_back(p2.z);
        z_components.push_back(p3.z);
      }

      /// Values for walls Histogram.///////////////////////////////////////////
      if ((FLAGS_only_use_non_clustered_points?
           !is_polygon_on_a_plane : true) &&
          isNormalPerpendicularToAxis(vertical, triangle_normal,
                                      normal_tolerance_walls)) {
        // WARNING if we do not normalize, we'll have two peaks for the same
        // plane, no?
        // Store theta.
        double theta = getLongitude(triangle_normal, vertical);

        // Store distance.
        // Using triangle_normal.
        double distance = p1.ddot(triangle_normal);
        if (theta < 0) {
          VLOG(10) << "Normalize theta: " << theta
                   << " and distance: " << distance;
          // Say theta is -pi/2, then normalized theta is pi/2.
          theta = theta + PI;
          // Change distance accordingly.
          distance = -distance;
          VLOG(10) << "New normalized theta: " << theta
                   << " and distance: " << distance;
        }
        walls.push_back(cv::Point2f(theta, distance));
        // WARNING should we instead be using projected triangle normal
        // on equator, and taking average of three distances...
        // NORMALIZE if a theta is positive and distance negative, it is the
        // same as if theta is 180 deg from it and distance positive...
      }
    }
  }

  VLOG(10) << "Number of polygons potentially on a wall: " << walls.rows;

  // Segment new planes.
  // Currently using lmks that were used by the seed_planes...
  segmentNewPlanes(new_planes, z_components, walls);
}

/* -------------------------------------------------------------------------- */
// Output goes from (-pi to pi], as we are using atan2, which looks at sign
// of arguments.
double Mesher::getLongitude(const cv::Point3f& triangle_normal,
                            const cv::Point3f& vertical) const {
  // A get projection on equatorial plane.
  // i get projection of triangle normal on vertical
  CHECK_NEAR(cv::norm(triangle_normal), 1.0, 1e-5); // Expect unit norm.
  CHECK_NEAR(cv::norm(vertical), 1.0, 1e-5); // Expect unit norm.
  cv::Point3f equatorial_proj = triangle_normal -
                                vertical.ddot(triangle_normal) * vertical;
  CHECK_NEAR(equatorial_proj.ddot(vertical), 0.0, 1e-5);
  CHECK(equatorial_proj.y != 0 || equatorial_proj.x != 0);
  return std::atan2(equatorial_proj.y, equatorial_proj.x);
}

/* -------------------------------------------------------------------------- */
// Update plane lmk ids field, by looping over the mesh and stoting lmk ids of
// the vertices of the polygons that are close to the plane.
// It will append lmk ids to the ones already present in the plane.
// Points with id vio, only used if we are using stereo points to build the mesh.
void Mesher::updatePlanesLmkIdsFromMesh(
    std::vector<Plane>* planes,
    double normal_tolerance, double distance_tolerance,
    const std::unordered_map<LandmarkId, gtsam::Point3>& points_with_id_vio)
const {
  CHECK_NOTNULL(planes);
  static constexpr size_t mesh_polygon_dim = 3;
  CHECK_EQ(mesh_.getMeshPolygonDimension(), mesh_polygon_dim)
      << "Expecting 3 vertices in triangle.";
  Mesh3D::Polygon polygon;
  for (size_t i = 0; i < mesh_.getNumberOfPolygons(); i++) {
    CHECK(mesh_.getPolygon(i, &polygon)) << "Could not retrieve polygon.";
    CHECK_EQ(polygon.size(), mesh_polygon_dim);
    const Mesh3D::VertexPosition3D& p1 = polygon.at(0).getVertexPosition();
    const Mesh3D::VertexPosition3D& p2 = polygon.at(1).getVertexPosition();
    const Mesh3D::VertexPosition3D& p3 = polygon.at(2).getVertexPosition();

    // Calculate normal of the triangle in the mesh.
    // The normals are in the world frame of reference.
    cv::Point3f triangle_normal;
    if (calculateNormal(p1, p2, p3, &triangle_normal)) {
      // Loop over newly segmented planes, and update lmk ids field if
      // the current polygon is on the plane.
      updatePlanesLmkIdsFromPolygon(planes,
                                    polygon,
                                    i, triangle_normal,
                                    normal_tolerance,
                                    distance_tolerance,
                                    points_with_id_vio);
    }
  }
}

/* -------------------------------------------------------------------------- */
// Updates planes lmk ids field with a polygon vertices ids if this polygon
// is part of the plane according to given tolerance.
// points_with_id_vio is only used if we are using stereo points...
bool Mesher::updatePlanesLmkIdsFromPolygon(
    std::vector<Plane>* seed_planes,
    const Mesh3D::Polygon& polygon,
    const size_t& triangle_id,
    const cv::Point3f& triangle_normal,
    double normal_tolerance, double distance_tolerance,
    const std::unordered_map<LandmarkId, gtsam::Point3>& points_with_id_vio)
const {
  CHECK_NOTNULL(seed_planes);
  bool is_polygon_on_a_plane = false;
  for (Plane& seed_plane: *seed_planes) {
    // Only cluster if normal and distance of polygon are close to plane.
    // WARNING: same polygon is being possibly clustered in multiple planes.
    // Break loop when polygon is in a plane?
    if (isNormalAroundAxis(seed_plane.normal_,
                           triangle_normal,
                           normal_tolerance) &&
        isPolygonAtDistanceFromPlane(polygon,
                                     seed_plane.distance_,
                                     seed_plane.normal_,
                                     distance_tolerance)) {
      // I guess we can comment out the check below since it can happen that
      // a poygon is segmented in two very close planes? Better we enable
      // it again!
      //CHECK(!is_polygon_on_a_plane) << "Polygon was already in a plane,"
      //                                 " are we having similar planes?";

      // Update lmk_ids of seed plane.
      // Points_with_id_vio are only used for stereo.
      appendLmkIdsOfPolygon(polygon, &seed_plane.lmk_ids_,
                            points_with_id_vio);

      // TODO Remove, only used for visualization...
      seed_plane.triangle_cluster_.triangle_ids_.push_back(triangle_id);

      // Acknowledge that the polygon is at least in one plane, to avoid
      // sending this polygon to segmentation and segment the same plane
      // again.
      is_polygon_on_a_plane = true;
    }
  }
  return is_polygon_on_a_plane;
}

/* -------------------------------------------------------------------------- */
// Segment new planes in the mesh.
// Currently segments horizontal planes using z_components, which is
// expected to be a cv::Mat z_components (1, 0, CV_32F);
// And walls perpendicular to the ground, using a cv::Mat which is expected to be
// a cv::Mat walls (0, 0, CV_32FC2), with first channel being theta (yaw angle of
// the wall) and the second channel the distance of it.
// points_with_id_vio is only used if we are using stereo points...
void Mesher::segmentNewPlanes(
    std::vector<Plane>* new_segmented_planes,
    const cv::Mat& z_components,
    const cv::Mat& walls) {
  CHECK_NOTNULL(new_segmented_planes);
  new_segmented_planes->clear();

  // Segment horizontal planes.
  static size_t plane_id = 0;
  static const Plane::Normal vertical (0, 0, 1);
  segmentHorizontalPlanes(new_segmented_planes,
                          &plane_id,
                          vertical,
                          z_components);

  // Segment vertical planes.
  segmentWalls(new_segmented_planes,
               &plane_id,
               walls);
}

/* -------------------------------------------------------------------------- */
// Segment wall planes.
// plane_id, starting id for new planes, it gets increased every time we add a
// new plane.
void Mesher::segmentWalls(std::vector<Plane>* wall_planes,
                          size_t* plane_id,
                          const cv::Mat& walls) {
  CHECK_NOTNULL(wall_planes);
  CHECK_NOTNULL(plane_id);
  ////////////////////////////// 2D Histogram //////////////////////////////////
  VLOG(10) << "Starting to calculate 2D histogram...";
  hist_2d_.calculateHistogram(walls);
  VLOG(10) << "Finished to calculate 2D histogram.";

  /// Added by me
  //cv::GaussianBlur(histImg, histImg, cv::Size(9, 9), 0);
  ///
  VLOG(10) << "Starting get local maximum for 2D histogram...";
  std::vector<Histogram::PeakInfo2D> peaks2;
  static const cv::Size kernel_size_2d (FLAGS_hist_2d_gaussian_kernel_size,
                                        FLAGS_hist_2d_gaussian_kernel_size);
  hist_2d_.getLocalMaximum2D(&peaks2, kernel_size_2d,
                            FLAGS_hist_2d_nr_of_local_max,
                            FLAGS_hist_2d_min_support,
                            FLAGS_hist_2d_min_dist_btw_local_max,
                            FLAGS_visualize_histogram_2D);
  VLOG(10) << "Finished get local maximum for 2D histogram.";

  VLOG(0) << "# of peaks in 2D histogram = " << peaks2.size();
  size_t i = 0;
  for (const Histogram::PeakInfo2D& peak: peaks2) {
    double plane_theta = peak.x_value_;
    double plane_distance = peak.y_value_;
    cv::Point3f plane_normal (std::cos(plane_theta), std::sin(plane_theta), 0);
    VLOG(0) << "Peak #" << i << " in bin with coords: "
            << " x= " << peak.pos_.x << " y= " << peak.pos_.y
            << ". So peak with theta = " << plane_theta
            << " (normal: x= " << plane_normal.x
            << " and y= " << plane_normal.y << " )"
            << " and distance = " << plane_distance;

    // WARNING we are not giving lmk ids to this plane!
    // We should either completely customize the histogram calc to pass lmk ids
    // or do another loop over the mesh to cluster new triangles.
    const gtsam::Symbol plane_symbol ('P', *plane_id);
    static constexpr int cluster_id = 1; // Only used for visualization. 1 = walls.
    VLOG(10) << "Segmented a wall plane with:\n"
             <<"\t normal: " << plane_normal
             <<"\t distance: " << plane_distance
            << "\n\t plane id: " << gtsam::DefaultKeyFormatter(plane_symbol.key())
            << "\n\t cluster id: " << cluster_id;
    wall_planes->push_back(Plane(plane_symbol,
                                 plane_normal,
                                 plane_distance,
                                 // Currently filled after this function...
                                 LandmarkIds(), // We should fill this!!!
                                 cluster_id));
    (*plane_id)++; // CRITICAL TO GET THIS RIGHT: ensure no duplicates,

    i++;
  }
}

/* -------------------------------------------------------------------------- */
// Segment new planes horizontal.
// plane_id, starting id for new planes, it gets increased every time we add a
// new plane.
void Mesher::segmentHorizontalPlanes(
    std::vector<Plane>* horizontal_planes,
    size_t* plane_id,
    const Plane::Normal& normal,
    const cv::Mat& z_components) {
  CHECK_NOTNULL(horizontal_planes);
  CHECK_NOTNULL(plane_id);
  ////////////////////////////// 1D Histogram //////////////////////////////////
  VLOG(10) << "Starting calculate 1D histogram.";
  z_hist_.calculateHistogram(z_components);
  VLOG(10) << "Finished calculate 1D histogram.";

  VLOG(10) << "Starting get local maximum for 1D.";
  static const cv::Size kernel_size (1, FLAGS_z_histogram_gaussian_kernel_size);
  std::vector<Histogram::PeakInfo> peaks =
      z_hist_.getLocalMaximum1D(kernel_size,
                             FLAGS_z_histogram_window_size,
                             FLAGS_z_histogram_peak_per,
                             FLAGS_z_histogram_min_support,
                             FLAGS_visualize_histogram_1D);
  VLOG(10) << "Finished get local maximum for 1D.";

  LOG(WARNING) << "# of peaks in 1D histogram = " << peaks.size();
  size_t i = 0;
  std::vector<Histogram::PeakInfo>::iterator previous_peak_it;
  double previous_plane_distance = -DBL_MAX;
  for (std::vector<Histogram::PeakInfo>::iterator peak_it = peaks.begin();
       peak_it != peaks.end();) {
    // Make sure it is below min possible value for distance.
    double plane_distance = peak_it->value_;
    VLOG(10) << "Peak #" << i << " in bin " << peak_it->pos_
             << " has distance = " << plane_distance
             << " with a support of " << peak_it->support_ << " points";

    // Remove duplicates, and, for peaks that are too close, take the one with
    // maximum support.
    // Assuming repeated peaks are ordered...
    if (i > 0 && *peak_it == peaks.at(i - 1)) {
      // Repeated element, delete it.
      LOG(WARNING) << "Deleting repeated peak for peak # " << i << " in bin "
                   << peak_it->pos_;
      peak_it = peaks.erase(peak_it);
      i--;
    } else if (i > 0 && std::fabs(previous_plane_distance - plane_distance) <
               FLAGS_z_histogram_min_separation) {
      // Not enough separation between planes, delete the one with less support.
      if (previous_peak_it->support_ < peak_it->support_) {
        // Delete previous_peak.
        LOG(WARNING) << "Deleting peak in bin " << previous_peak_it->pos_;
        //Iterators, pointers and references pointing to position (or first) and
        // beyond are invalidated, with all iterators, pointers and references
        // to elements before position (or first) are guaranteed to keep
        // referring to the same elements they were referring to before the call.
        peaks.erase(previous_peak_it);
        peak_it = peaks.begin() + i - 1;
        i--;
      } else {
        // Delete peak_it.
        LOG(WARNING) << "Deleting too close peak # " << i << " in bin "
                     << peak_it->pos_;
        peak_it = peaks.erase(peak_it);
        i--;
      }
    } else {
      previous_peak_it = peak_it;
      previous_plane_distance = plane_distance;
      peak_it++;
      i++;
    }
  }

  for (int peak_nr = 0; peak_nr < FLAGS_z_histogram_max_number_of_peaks_to_select;
       peak_nr++) {
    // Get the peaks in order of max support.
    std::vector<Histogram::PeakInfo>::iterator it =
        std::max_element(peaks.begin(), peaks.end());
    if (it != peaks.end()) {
      double plane_distance = it->value_;
      // WARNING we are not giving lmk ids to this plane!
      // We should either completely customize the histogram calc to pass lmk ids
      // or do another loop over the mesh to cluster new triangles.
      const gtsam::Symbol plane_symbol ('P', *plane_id);
      static constexpr int cluster_id = 2; // Only used for visualization. 2 = ground.
      VLOG(10) << "Segmented an horizontal plane with:\n"
               <<"\t distance: " << plane_distance << "\n\t plane id: "
               << gtsam::DefaultKeyFormatter(plane_symbol.key())
               << "\n\t cluster id: " << cluster_id;
      horizontal_planes->push_back(Plane(plane_symbol,
                                         normal,
                                         plane_distance,
                                         // Currently filled after this function...
                                         LandmarkIds(), // We should fill this!!!
                                         cluster_id));
      (*plane_id)++; // CRITICAL TO GET THIS RIGHT: ensure no duplicates,
      // no wrong ids...

      // Delete current peak from set of peaks, so that we can find next maximum.
      peaks.erase(it);
    } else {
      if (peaks.size() == 0) {
        VLOG(10) << "No more peaks available.";
      } else {
        VLOG(10) << "Could not find a maximum among the list of " << peaks.size()
                << " peaks in histogram of horizontal planes.";
      }
      break;
    }
  }
}

/* -------------------------------------------------------------------------- */
// Data association between planes.
void Mesher::associatePlanes(const std::vector<Plane>& segmented_planes,
                             const std::vector<Plane>& planes,
                             std::vector<Plane>* non_associated_planes,
                             const double& normal_tolerance,
                             const double& distance_tolerance) const {
  CHECK_NOTNULL(non_associated_planes);
  non_associated_planes->clear();
  if (planes.size() == 0) {
    // There are no previous planes, data association unnecessary, just copy
    // segmented planes to output planes.
    VLOG(0) << "No planes in backend, just copy the "
            << segmented_planes.size() << " segmented planes to the set of "
            << "backend planes, skipping data association.";
    *non_associated_planes = segmented_planes;
  } else {
    // Planes tmp will contain the new segmented planes.
    // Both the ones that could be associated, in which case only the landmark
    // ids are updated (symbol, norm, distance remain the same).
    // WARNING maybe we should do the Union of both the lmk ids of the new plane
    // and the old plane........................
    // And the ones that could not be associated, in which case they are added
    // as new planes, with a new symbol.
    if (segmented_planes.size() == 0) {
      LOG(WARNING) << "No segmented planes.";
    }
    //std::vector<Plane> planes_tmp;
    // To avoid  associating several segmented planes to the same
    // plane_backend
    std::vector<uint64_t> associated_plane_ids;
    for (const Plane& segmented_plane: segmented_planes) {
      bool is_segmented_plane_associated = false;
      for (const Plane& plane_backend: planes) {
        // Check if normals are close or 180 degrees apart.
        // Check if distance is similar in absolute value.
        // TODO check distance given the difference in normals.
        if (plane_backend.geometricEqual(segmented_plane,
                                         normal_tolerance,
                                         distance_tolerance)) {
          // We found a plane association
          uint64_t backend_plane_index = plane_backend.getPlaneSymbol().index();
          // Check that it was not associated before.
          if (std::find(associated_plane_ids.begin(),
                        associated_plane_ids.end(), backend_plane_index) ==
              associated_plane_ids.end()) {
            // It is the first time we associate this plane.
            // Update lmk ids in plane.
            VLOG(10)
                << "Plane from backend with id " << gtsam::DefaultKeyFormatter(
                     plane_backend.getPlaneSymbol().key())
                << " has been associated with segmented plane: "
                << gtsam::DefaultKeyFormatter(
                     segmented_plane.getPlaneSymbol().key());
            // Update plane.
            // WARNING maybe do the union between previous and current lmk_ids?
            // Or just don't do anything, cause the plane should be there with
            // its own lmk ids already...
            // Actually YES DO IT, so we can spare one loop over the mesh!
            // the first one ! (aka go back to the so-called naive implementation!
            // Not entirely true, since we still need to loop over the mesh to get
            // the points for extracting new planes...
            //plane_backend.lmk_ids_ = segmented_plane.lmk_ids_;
            // WARNING TODO should we also update the normal & distance??
            // Acknowledge that we have an association.
            associated_plane_ids.push_back(backend_plane_index);

            is_segmented_plane_associated = true;
            break;
          } else {
            // Continue, to see if we can associate the current segmented plane
            // to another backend plane.
            LOG(ERROR) << "Double plane association of backend plane: "
                       << gtsam::DefaultKeyFormatter(
                            plane_backend.getPlaneSymbol().key())
                       << " with another segmented plane: "
                       << gtsam::DefaultKeyFormatter(
                            segmented_plane.getPlaneSymbol().key()) << "\n.";
            if (FLAGS_do_double_association) {
              LOG(ERROR) << "Doing double plane association of backend plane.";
              is_segmented_plane_associated = true;
              break;
            } else {
              LOG(ERROR)
                   << "Avoiding double plane association of backend plane. "
                   << "Searching instead for another possible backend plane for this"
                      " segmented plane.";
              continue;
            }
          }
        } else {
          VLOG(0)
              << "Plane " << gtsam::DefaultKeyFormatter(
                   plane_backend.getPlaneSymbol().key())
              << " from backend not associated to new segmented plane "
              << gtsam::DefaultKeyFormatter(segmented_plane.getPlaneSymbol())
              << "\n\tSegmented normal: " << segmented_plane.normal_
              << " ( vs normal: " << plane_backend.normal_
              << ") \n\tSegmented distance: " << segmented_plane.distance_
              << " ( vs distance: " << plane_backend.distance_ << ").";
        }
      }

      if (!is_segmented_plane_associated) {
        // The segmented plane could not be associated to any existing plane
        // in the backend...
        // Add it as a new plane.
        VLOG(0) << "Add plane with id "
                 << gtsam::DefaultKeyFormatter(segmented_plane.getPlaneSymbol())
                 << " as a new plane for the backend.";
        // WARNING by pushing we are also associating segmented planes between
        // them, do we want this? Maybe yes because we have some repeated
        // segmented planes that are very similar, but then what's up with the
        // lmk ids, which should we keep?
        non_associated_planes->push_back(segmented_plane);
      }
    }

    // Update planes.
    // Cleans planes that have not been updated
    // Which is not necessarily good, because the segmenter might differ from
    // backend and we will not do expectation maximization anymore... plus
    // it makes the visualizer not work...
    //*planes = planes_tmp;
  }
}

/* -------------------------------------------------------------------------- */
// Update mesh: update structures keeping memory of the map before visualization
void Mesher::updateMesh3D(
    const std::unordered_map<LandmarkId, gtsam::Point3>& points_with_id_VIO,
    std::shared_ptr<StereoFrame> stereoFrame,
    const gtsam::Pose3& leftCameraPose,
    cv::Mat* mesh_2d_img) {
  VLOG(10) << "Starting updateMesh3D...";
  const std::unordered_map<LandmarkId, gtsam::Point3>* points_with_id_all =
      &points_with_id_VIO;

  // Get points in stereo camera that are not in vio but have lmk id:
  std::unordered_map<LandmarkId, gtsam::Point3> points_with_id_stereo;
  if (FLAGS_add_extra_lmks_from_stereo) {
    // Append vio points.
    // WARNING some stereo and vio lmks share the same id, so adding order matters!
    // first add vio points, then stereo, so that vio points have preference
    // over stereo ones if they are repeated!
    points_with_id_stereo = points_with_id_VIO;
    appendNonVioStereoPoints(stereoFrame,
                             leftCameraPose,
                             &points_with_id_stereo);
    VLOG(20) << "Number of stereo landmarks used for the mesh: "
             << points_with_id_stereo.size() << "\n"
             << "Number of VIO landmarks used for the mesh: "
             << points_with_id_VIO.size();

    points_with_id_all = &points_with_id_stereo;
  }
  VLOG(20) << "Total number of landmarks used for the mesh: "
           << points_with_id_all->size();

  // Build 2D mesh.
  std::vector<cv::Vec6f> mesh_2d;
  stereoFrame->createMesh2dVIO(&mesh_2d,
                               *points_with_id_all);
  std::vector<cv::Vec6f> mesh_2d_filtered;
  stereoFrame->filterTrianglesWithGradients(mesh_2d,
                                            &mesh_2d_filtered,
                                            FLAGS_max_grad_in_triangle);

  // Debug.
  if (FLAGS_visualize_mesh_2d) {
    stereoFrame->visualizeMesh2DStereo(mesh_2d, mesh_2d_img);
  } else if (FLAGS_visualize_mesh_2d_filtered) {
    stereoFrame->visualizeMesh2DStereo(mesh_2d_filtered, mesh_2d_img,
                                       "2D Mesh Filtered");
  }

  populate3dMeshTimeHorizon(
        mesh_2d_filtered,
        *points_with_id_all,
        stereoFrame->left_frame_,
        leftCameraPose,
        FLAGS_min_ratio_btw_largest_smallest_side,
        FLAGS_min_elongation_ratio,
        FLAGS_max_triangle_side);

  VLOG(10) << "Finished updateMesh3D.";
}


/* -------------------------------------------------------------------------- */
// Attempts to insert new points in the map, but does not override if there
// is already a point with the same lmk id.
void Mesher::appendNonVioStereoPoints(
    std::shared_ptr<StereoFrame> stereoFrame,
    const gtsam::Pose3& leftCameraPose,
    std::unordered_map<LandmarkId, gtsam::Point3>* points_with_id_stereo) const {
  CHECK_NOTNULL(points_with_id_stereo);
  const Frame& leftFrame = stereoFrame->left_frame_;
  for (size_t i = 0; i < leftFrame.landmarks_.size(); i++) {
    if (stereoFrame->right_keypoints_status_.at(i) == Kstatus::VALID &&
        leftFrame.landmarks_.at(i) != -1) {
      const gtsam::Point3& p_i_global =
          leftCameraPose.transform_from(gtsam::Point3(
                                          stereoFrame->keypoints_3d_.at(i)));
      // Use insert() instead of [] operator, to make sure that if there is
      // already a point with the same lmk_id, we do not override it.
      points_with_id_stereo->insert(std::make_pair(
                                      leftFrame.landmarks_.at(i),
                                      p_i_global));
    }
  }
}

/* -------------------------------------------------------------------------- */
// TODO avoid this loop by enforcing to pass the lmk id of the vertex of the
// triangle in the triangle cluster.
// In case we are using extra lmks from stereo,
// then it makes sure that the lmk ids are used in the optimization
// (they are present in time horizon).
void Mesher::extractLmkIdsFromVectorOfTriangleClusters(
    const std::vector<TriangleCluster>& triangle_clusters,
    const std::unordered_map<LandmarkId, gtsam::Point3>& points_with_id_vio,
    LandmarkIds* lmk_ids) const {
  VLOG(10) << "Starting extract lmk ids for vector of triangle cluster...";
  CHECK_NOTNULL(lmk_ids);
  lmk_ids->resize(0);

  for (const TriangleCluster& triangle_cluster: triangle_clusters) {
    extractLmkIdsFromTriangleCluster(triangle_cluster,
                                     points_with_id_vio,
                                     lmk_ids);
  }
  VLOG(10) << "Finished extract lmk ids for vector of triangle cluster.";
}

/* -------------------------------------------------------------------------- */
// Extracts lmk ids from triangle cluster. In case we are using extra lmks
// from stereo, then it makes sure that the lmk ids are used in the optimization
// (they are present in time horizon: meaning it checks that we can find the
// lmk id in points_with_id_vio...
void Mesher::extractLmkIdsFromTriangleCluster(
    const TriangleCluster& triangle_cluster,
    const std::unordered_map<LandmarkId, gtsam::Point3>& points_with_id_vio,
    LandmarkIds* lmk_ids) const {
  VLOG(10) << "Starting extractLmkIdsFromTriangleCluster...";
  CHECK_NOTNULL(lmk_ids);
  lmk_ids->resize(0);

  Mesh3D::Polygon polygon;
    for (const size_t& polygon_idx: triangle_cluster.triangle_ids_) {
      CHECK(mesh_.getPolygon(polygon_idx, &polygon))
          << "Polygon, with idx " << polygon_idx << ", is not in the mesh.";
      appendLmkIdsOfPolygon(polygon, lmk_ids, points_with_id_vio);
    }
  VLOG(10) << "Finished extractLmkIdsFromTriangleCluster.";
}

/* -------------------------------------------------------------------------- */
// Extracts lmk ids from a mesh polygon and appends them to lmk_ids.
// In case we are using extra lmks from stereo, then it makes sure that the lmk
// ids are used in the optimization (they are present in time horizon: meaning
// it checks that we can find the lmk id in points_with_id_vio...
// WARNING: this function won't check that the original lmk_ids are in the
// optimization (time-horizon)...
void Mesher::appendLmkIdsOfPolygon(
    const Mesh3D::Polygon& polygon,
    LandmarkIds* lmk_ids,
    const std::unordered_map<LandmarkId, gtsam::Point3>& points_with_id_vio)
const {
  CHECK_NOTNULL(lmk_ids);
  for (const Mesh3D::Vertex& vertex: polygon) {
    // Ensure we are not adding more than once the same lmk_id.
    const auto& it = std::find(lmk_ids->begin(),
                               lmk_ids->end(),
                               vertex.getLmkId());
    if (it == lmk_ids->end()) {
      // The lmk id is not present in the lmk_ids vector, add it.
      if (FLAGS_add_extra_lmks_from_stereo) {
        // Only add lmks that are used in the backend (time-horizon).
        // This is just needed when adding extra lmks from stereo...
        // We are assuming lmk_ids has already only points in time-horizon,
        // so no need to check them as well.
        if (points_with_id_vio.find(vertex.getLmkId()) !=
            points_with_id_vio.end()) {
          lmk_ids->push_back(vertex.getLmkId());
        }
      } else {
        lmk_ids->push_back(vertex.getLmkId());
      }
    } else {
      // The lmk id is already in the lmk_ids vector, do not add it.
      continue;
    }
  }
}


/* -------------------------------------------------------------------------- */
void Mesher::getVerticesMesh(cv::Mat* vertices_mesh) const {
  CHECK_NOTNULL(vertices_mesh);
  mesh_.convertVerticesMeshToMat(vertices_mesh);
}
void Mesher::getPolygonsMesh(cv::Mat* polygons_mesh) const {
  CHECK_NOTNULL(polygons_mesh);
  mesh_.convertPolygonsMeshToMat(polygons_mesh);
}

} // namespace VIO

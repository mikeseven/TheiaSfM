// Copyright (C) 2014 The Regents of the University of California (Regents).
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//
//     * Neither the name of The Regents or University of California nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Please contact the author of this library if you have any questions.
// Author: Chris Sweeney (cmsweeney@cs.ucsb.edu)

#include "theia/sfm/reconstruction_builder.h"

#include <glog/logging.h>
#include <memory>
#include <string>
#include <vector>

#include "theia/image/descriptor/binary_descriptor.h"
#include "theia/sfm/camera_intrinsics_prior.h"
#include "theia/sfm/exif_reader.h"
#include "theia/sfm/feature_extractor.h"
#include "theia/sfm/match_and_verify_features.h"
#include "theia/sfm/reconstruction.h"
#include "theia/sfm/reconstruction_estimator.h"
#include "theia/sfm/track_builder.h"
#include "theia/sfm/types.h"
#include "theia/sfm/view.h"
#include "theia/sfm/view_graph/view_graph.h"
#include "theia/util/filesystem.h"

namespace theia {

namespace {

inline bool IsFloatDescriptor(const DescriptorExtractorType& descriptor_type) {
  if (descriptor_type == DescriptorExtractorType::SIFT) {
    return true;
  }
  return false;
}

Reconstruction* CreateEstimatedSubreconstruction(
    const Reconstruction& input_reconstruction) {
  std::unique_ptr<Reconstruction> subreconstruction(
      new Reconstruction(input_reconstruction));
  const auto& view_ids = subreconstruction->ViewIds();
  for (const ViewId view_id : view_ids) {
    const View* view = subreconstruction->View(view_id);
    if (view == nullptr) {
      continue;
    }

    if (!view->IsEstimated()) {
      subreconstruction->RemoveView(view_id);
    }
  }

  const auto& track_ids = subreconstruction->TrackIds();
  for (const TrackId track_id : track_ids) {
    const Track* track = subreconstruction->Track(track_id);
    if (track == nullptr) {
      continue;
    }

    if (!track->IsEstimated()) {
      subreconstruction->RemoveTrack(track_id);
    }
  }
  return subreconstruction.release();
}

void RemoveEstimatedViewsAndTracks(Reconstruction* reconstruction,
                                   ViewGraph* view_graph) {
  const auto& view_ids = reconstruction->ViewIds();
  for (const ViewId view_id : view_ids) {
    const View* view = reconstruction->View(view_id);
    if (view == nullptr) {
      continue;
    }

    if (view->IsEstimated()) {
      reconstruction->RemoveView(view_id);
      view_graph->RemoveView(view_id);
    }
  }

  const auto& track_ids = reconstruction->TrackIds();
  for (const TrackId track_id : track_ids) {
    const Track* track = reconstruction->Track(track_id);
    if (track == nullptr) {
      continue;
    }

    if (track->IsEstimated()) {
      reconstruction->RemoveTrack(track_id);
    }
  }
}

}  // namespace

ReconstructionBuilder::ReconstructionBuilder(
    const ReconstructionBuilderOptions& options)
    : options_(options) {
  CHECK_GT(options.num_threads, 0);

  reconstruction_.reset(new Reconstruction());
  view_graph_.reset(new ViewGraph());
  track_builder_.reset(new TrackBuilder(options.max_track_length));
  exif_reader_.reset(new ExifReader());
}

ReconstructionBuilder::~ReconstructionBuilder() {}

bool ReconstructionBuilder::AddImage(const std::string& image_filepath) {
  // Extract the metadata. If this method fails it means the file does not
  // exist.
  CameraIntrinsicsPrior camera_intrinsics_prior;
  CHECK(exif_reader_->ExtractEXIFMetadata(image_filepath,
                                          &camera_intrinsics_prior))
      << "Could not extract camera_intrinsics_prior from " << image_filepath;
  return AddImageWithCameraIntrinsicsPrior(image_filepath,
                                          camera_intrinsics_prior);
}

bool ReconstructionBuilder::AddImageWithCameraIntrinsicsPrior(
    const std::string& image_filepath,
    const CameraIntrinsicsPrior& camera_intrinsics_prior) {
  image_filepaths_.emplace_back(image_filepath);
  camera_intrinsics_priors_.emplace_back(camera_intrinsics_prior);

  std::string image_file;
  CHECK(GetFilenameFromFilepath(image_filepath, true, &image_file));

  // Add the image to the reconstruction.
  const ViewId view_id = reconstruction_->AddView(image_file);
  if (view_id == kInvalidViewId) {
    LOG(INFO) << "Could not add " << image_file << " to the reconstruction.";
    return false;
  }

  View* view = reconstruction_->MutableView(view_id);
  *view->MutableCameraIntrinsicsPrior() = camera_intrinsics_prior;
  view->MutableCamera()->SetImageSize(
      view->CameraIntrinsicsPrior().image_width,
      view->CameraIntrinsicsPrior().image_height);

  // Set the principal point from the camera priors if it exists otherwise
  // assume it is at the center of the image.
  if (view->CameraIntrinsicsPrior().principal_point[0].is_set &&
      view->CameraIntrinsicsPrior().principal_point[1].is_set) {
    view->MutableCamera()->SetPrincipalPoint(
        view->CameraIntrinsicsPrior().principal_point[0].value,
        view->CameraIntrinsicsPrior().principal_point[1].value);
  } else {
    view->MutableCamera()->SetPrincipalPoint(
        view->CameraIntrinsicsPrior().image_width / 2.0,
        view->CameraIntrinsicsPrior().image_height / 2.0);
  }

  if (view->CameraIntrinsicsPrior().focal_length.is_set) {
    LOG(INFO) << "Adding image " << image_file
              << " to reconstruction with focal length: "
              << camera_intrinsics_prior.focal_length.value;
  } else {
    LOG(INFO) << "Adding image " << image_file
              << " to reconstruction with focal length: UNKNOWN";
  }
  return true;
}

bool ReconstructionBuilder::ExtractAndMatchFeatures() {
  CHECK_EQ(view_graph_->NumViews(), 0) << "Cannot call ExtractAndMatchFeatures "
                                          "after TwoViewMatches has been "
                                          "called.";

  std::vector<std::vector<Keypoint>*> keypoints;
  FeatureExtractorOptions feature_extractor_options;
  feature_extractor_options.num_threads = options_.num_threads;
  feature_extractor_options.descriptor_extractor_type =
      options_.descriptor_type;
  FeatureExtractor feature_extractor(feature_extractor_options);

  bool success = false;
  if (IsFloatDescriptor(options_.descriptor_type)) {
    std::vector<std::vector<Eigen::VectorXf>*> float_descriptors;

    // Extract and match floating point descriptors.
    LOG(INFO) << "Extracting features.";
    CHECK(feature_extractor.Extract(image_filepaths_,
                                    &keypoints,
                                    &float_descriptors))
        << "Could not extract features.";

    LOG(INFO) << "Matching features.";
    success = MatchFeatures(keypoints, float_descriptors);
    STLDeleteElements(&float_descriptors);
  } else {
    std::vector<std::vector<BinaryVectorX>*> binary_descriptors;

    LOG(INFO) << "Extracting features.";
    // Extract and match binary descriptors.
    CHECK(feature_extractor.Extract(image_filepaths_,
                                    &keypoints,
                                    &binary_descriptors))
        << "Could not extract features.";

    LOG(INFO) << "Matching features.";
    success = MatchFeatures(keypoints, binary_descriptors);
    STLDeleteElements(&binary_descriptors);
  }

  // Clean up allocated data.
  STLDeleteElements(&keypoints);

  return success;
}

bool ReconstructionBuilder::AddTwoViewMatch(
    const std::string& image1, const std::string& image2,
    const ImagePairMatch& matches) {
  // Add valid matches to view graph.
  AddMatchToViewGraph(image1, image2, matches);

  // Add tracks to the track builder.
  AddTracksForMatch(image1, image2, matches);

  return true;
}

void ReconstructionBuilder::CameraIntrinsicsFromCameraIntrinsicsPriors(
    std::vector<CameraIntrinsics>* intrinsics) const {
  CHECK_NOTNULL(intrinsics)->resize(camera_intrinsics_priors_.size());
  for (int i = 0; i < intrinsics->size(); i++) {
    // Set focal length.
    if (camera_intrinsics_priors_[i].focal_length.is_set) {
      (*intrinsics)[i].focal_length =
          camera_intrinsics_priors_[i].focal_length.value;
    }

    // Set principal point.
    if (camera_intrinsics_priors_[i].principal_point[0].is_set &&
        camera_intrinsics_priors_[i].principal_point[1].is_set) {
      (*intrinsics)[i].principal_point[0] =
          camera_intrinsics_priors_[i].principal_point[0].value;
      (*intrinsics)[i].principal_point[1] =
          camera_intrinsics_priors_[i].principal_point[1].value;
    }
  }
}

bool ReconstructionBuilder::BuildReconstruction(
    std::vector<Reconstruction*>* reconstructions) {
  CHECK_GT(view_graph_->NumViews(), 0)
      << "You must add images with the reconstruction "
         "builder before calling BuildReconstruction.";

  track_builder_->BuildTracks(reconstruction_.get());

  while (reconstruction_->NumViews() > 2) {
    std::unique_ptr<ReconstructionEstimator> reconstruction_estimator(
        ReconstructionEstimator::Create(
            options_.reconstruction_estimator_options));

    const auto& summary =
        reconstruction_estimator->Estimate(*view_graph_, reconstruction_.get());

    // If a reconstruction can no longer be estimated, return.
    if (!summary.success) {
      return reconstructions->size() > 0;
    }

    LOG(INFO)
        << "Reconstruction estimation statistics: "
        << "\n\tNum estimated views = " << summary.estimated_views.size()
        << "\n\tNum input views = " << reconstruction_->NumViews()
        << "\n\tNum estimated tracks = " << summary.estimated_tracks.size()
        << "\n\tNum input tracks = " << reconstruction_->NumTracks()
        << "\n\tInitial view graph filtering time = "
        << summary.initial_view_graph_filtering_time
        << "\n\tCamera intrinsic calibration time = "
        << summary.camera_intrinsics_calibration_time
        << "\n\tRotation estimation time = " << summary.rotation_estimation_time
        << "\n\tRotation filtering time = " << summary.rotation_filtering_time
        << "\n\tRelative translation optimization time = "
        << summary.relative_translation_optimization_time
        << "\n\tRelative translation filtering time = "
        << summary.relative_translation_filtering_time
        << "\n\tPosition estimation time = " << summary.position_estimation_time
        << "\n\tTriangulation time = " << summary.triangulation_time
        << "\n\tBundle Adjustment time = " << summary.bundle_adjustment_time;

    // Remove estimated views and tracks and attempt to create a reconstruction
    // from the remaining unestimated parts.
    reconstructions->emplace_back(
        CreateEstimatedSubreconstruction(*reconstruction_));
    RemoveEstimatedViewsAndTracks(reconstruction_.get(), view_graph_.get());

    if (reconstruction_->NumViews() < 3) {
      LOG(INFO) << "No more reconstructions can be estimated.";
      return reconstructions->size() > 0;
    }
  }
  return true;
}

void ReconstructionBuilder::AddMatchToViewGraph(
    const std::string& image1,
    const std::string& image2,
    const ImagePairMatch& image_matches) {
  // Get view ids from names and check that the views are valid (i.e. that
  // they have been added to the reconstruction).
  const ViewId view_id_1 = reconstruction_->ViewIdFromName(image1);
  const ViewId view_id_2 = reconstruction_->ViewIdFromName(image2);
  CHECK_NE(view_id_1, kInvalidViewId)
      << "Tried to add a view with the name " << image1
      << " to the view graph but does not exist in the reconstruction.";
  CHECK_NE(view_id_2, kInvalidViewId)
      << "Tried to add a view with the name " << image2
      << " to the view graph but does not exist in the reconstruction.";

  // Add the view pair to the reconstruction. The view graph requires the two
  // view info
  // to specify the transformation from the smaller view id to the larger view
  // id. We swap the cameras here if that is not already the case.
  TwoViewInfo twoview_info = image_matches.twoview_info;
  if (view_id_1 > view_id_2) {
    SwapCameras(&twoview_info);
  }

  view_graph_->AddEdge(view_id_1, view_id_2, twoview_info);
}

void ReconstructionBuilder::AddTracksForMatch(
    const std::string& image1,
    const std::string& image2,
    const ImagePairMatch& matches) {
  const ViewId view_id1 = reconstruction_->ViewIdFromName(image1);
  const ViewId view_id2 = reconstruction_->ViewIdFromName(image2);

  for (const auto& match : matches.correspondences) {
    track_builder_->AddFeatureCorrespondence(view_id1, match.feature1,
                                             view_id2, match.feature2);
  }
}

}  // namespace theia

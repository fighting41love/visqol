// Copyright 2019 Google LLC, Andrew Hines
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "visqol_manager.h"

#include <memory>
#include <string>
#include <vector>

#include "absl/base/internal/raw_logging.h"
#include "absl/memory/memory.h"

#include "google/protobuf/stubs/status.h"
#include "google/protobuf/stubs/status_macros.h"

#include "alignment.h"
#include "analysis_window.h"
#include "audio_signal.h"
#include "gammatone_filterbank.h"
#include "misc_audio.h"
#include "neurogram_similiarity_index_measure.h"
#include "similarity_result.h"
#include "similarity_result.pb.h"  // Generated by cc_proto_library rule
#include "speech_similarity_to_quality_mapper.h"
#include "vad_patch_creator.h"
#include "visqol.h"

#include "google/protobuf/port_def.inc"
// This 'using' declaration is necessary for the ASSIGN_OR_RETURN macro.
using namespace google::protobuf::util;

namespace Visqol {

const size_t k16kSampleRate = 16000;
const size_t k48kSampleRate = 48000;
const size_t VisqolManager::kPatchSize = 30;
const size_t VisqolManager::kPatchSizeSpeech = 20;
const size_t VisqolManager::kNumBandsAudio = 32;
const size_t VisqolManager::kNumBandsSpeech = 21;
const double VisqolManager::kMinimumFreq = 50;  // wideband
const double VisqolManager::kOverlap = 0.25;  // 25% overlap
const double VisqolManager::kDurationMismatchTolerance = 1.0;

Status VisqolManager::Init(const FilePath sim_to_quality_mapper_model,
    const bool use_speech_mode, const bool use_unscaled_speech) {
  use_speech_mode_ = use_speech_mode;
  use_unscaled_speech_mos_mapping_ = use_unscaled_speech;
  InitPatchCreator();
  InitPatchSelector();
  InitSpectrogramBuilder();
  auto status = InitSimilarityToQualityMapper(sim_to_quality_mapper_model);

  if (status.ok()) {
    is_initialized_ = true;
  } else {
    ABSL_RAW_LOG(ERROR, "%s", status.error_message().ToString().c_str());
  }

  return status;
}

void VisqolManager::InitPatchCreator() {
  if (use_speech_mode_) {
    patch_creator_ = absl::make_unique<VadPatchCreator>(kPatchSizeSpeech);
  } else {
    patch_creator_ = absl::make_unique<ImagePatchCreator>(kPatchSize);
  }
}

void VisqolManager::InitPatchSelector() {
  // Setup the patch similarity comparator to use the Neurogram.
  patch_selector_ = absl::make_unique<ComparisonPatchesSelector>(
      absl::make_unique<NeurogramSimiliarityIndexMeasure>());
}

void VisqolManager::InitSpectrogramBuilder() {
  if (use_speech_mode_) {
    spectrogram_builder_ = absl::make_unique<GammatoneSpectrogramBuilder>(
        GammatoneFilterBank{kNumBandsSpeech, kMinimumFreq}, true);
  } else {
    spectrogram_builder_ = absl::make_unique<GammatoneSpectrogramBuilder>(
        GammatoneFilterBank{kNumBandsAudio, kMinimumFreq}, false);
  }
}

Status VisqolManager::InitSimilarityToQualityMapper(
    FilePath sim_to_quality_mapper_model) {
  if (use_speech_mode_) {
    sim_to_qual_ = absl::make_unique<SpeechSimilarityToQualityMapper>(
        !use_unscaled_speech_mos_mapping_);
  } else {
    sim_to_qual_ = absl::make_unique<SvrSimilarityToQualityMapper>(
      sim_to_quality_mapper_model);
  }
  return sim_to_qual_->Init();
}

std::vector<SimilarityResultMsg> VisqolManager::Run(
    const std::vector<ReferenceDegradedPathPair>& signals_to_compare) {
  std::vector<SimilarityResultMsg> sim_results;
  // Iterate over all signal pairs to compare.
  for (const auto &signal_pair : signals_to_compare) {
    // Run comparison on a single signal pair.
    auto status_or = Run(signal_pair.reference, signal_pair.degraded);
    // If successful save value, else log an error.
    if (status_or.ok()) {
      sim_results.push_back(status_or.ValueOrDie());
    } else {
      ABSL_RAW_LOG(ERROR,
          "Error executing ViSQOL: %s.", status_or.status().ToString().c_str());
      // A status of aborted gets thrown when visqol hasn't been init'd.
      // So if that happens we want to quit processing.
      if (status_or.status().error_code() == error::Code::ABORTED) {
        break;
      }
    }
  }
  return sim_results;
}

StatusOr<SimilarityResultMsg> VisqolManager::Run(
    const FilePath& ref_signal_path, const FilePath& deg_signal_path) {

  // Ensure the initialization succeeded.
  RETURN_IF_ERROR(ErrorIfNotInitialized());

  // Load the wav audio files as mono.
  const AudioSignal ref_signal = MiscAudio::LoadAsMono(ref_signal_path);
  AudioSignal deg_signal = MiscAudio::LoadAsMono(deg_signal_path);

  // If the sim result was successfully calculated, set the signal file paths.
  // Else, return the StatusOr failure.
  SimilarityResultMsg sim_result_msg;
  ASSIGN_OR_RETURN(sim_result_msg, Run(ref_signal, deg_signal));
  sim_result_msg.set_reference_filepath(ref_signal_path.Path());
  sim_result_msg.set_degraded_filepath(deg_signal_path.Path());
  return sim_result_msg;
}

StatusOr<SimilarityResultMsg> VisqolManager::Run(
    const AudioSignal& ref_signal, AudioSignal& deg_signal) {

  // Ensure the initialization succeeded.
  RETURN_IF_ERROR(ErrorIfNotInitialized());

  RETURN_IF_ERROR(ValidateInputAudio(ref_signal, deg_signal));

  // Adjust for codec initial padding.
  auto alignment_result = Alignment::GloballyAlign(ref_signal, deg_signal);
  deg_signal = std::get<0>(alignment_result);

  const AnalysisWindow window{ref_signal.sample_rate, kOverlap};

  // If the sim result is successfully calculated, populate the protobuf msg.
  // Else, return the StatusOr failure.
  const Visqol visqol;
  SimilarityResult sim_result;
  ASSIGN_OR_RETURN(sim_result, visqol.CalculateSimilarity(ref_signal,
      deg_signal, spectrogram_builder_.get(), window, patch_creator_.get(),
      patch_selector_.get(), sim_to_qual_.get()));
  return PopulateSimResultMsg(sim_result);
}

SimilarityResultMsg VisqolManager::PopulateSimResultMsg(
      const SimilarityResult &sim_result) {
  SimilarityResultMsg sim_result_msg;
  sim_result_msg.set_moslqo(sim_result.moslqo);
  sim_result_msg.set_vnsim(sim_result.vnsim);

  auto fvnsim = sim_result.fvnsim;
  for (auto itr = fvnsim.begin(); itr != fvnsim.end(); ++itr) {
    sim_result_msg.add_fvnsim(*itr);
  }

  auto cfb = sim_result.center_freq_bands;
  for (auto itr = cfb.begin(); itr != cfb.end(); ++itr) {
    sim_result_msg.add_center_freq_bands(*itr);
  }

  for (auto patch : sim_result.debug_info.patch_sims) {
    auto patch_msg = sim_result_msg.add_patch_sims();
    patch_msg->set_similarity(patch.similarity);
    patch_msg->set_ref_patch_start_time(patch.ref_patch_start_time);
    patch_msg->set_ref_patch_end_time(patch.ref_patch_end_time);
    patch_msg->set_deg_patch_start_time(patch.deg_patch_start_time);
    patch_msg->set_deg_patch_end_time(patch.deg_patch_end_time);
    for (double each_fbm : patch.freq_band_means.ToVector()) {
      patch_msg->add_freq_band_means(each_fbm);
    }
  }

  return sim_result_msg;
}

Status VisqolManager::ErrorIfNotInitialized() {
  if (is_initialized_ == false) {
    return Status(error::Code::ABORTED,
        "VisqolManager must be initialized before use.");
  } else {
    return Status();
  }
}

google::protobuf::util::Status VisqolManager::ValidateInputAudio(
      const AudioSignal& ref_signal, const AudioSignal& deg_signal) {
  // Warn if there is an excessive difference in durations.
  double ref_duration = ref_signal.GetDuration();
  double deg_duration = deg_signal.GetDuration();
  if (std::abs(ref_duration - deg_duration) > kDurationMismatchTolerance) {
    ABSL_RAW_LOG(WARNING, "Mismatch in duration between reference and "
      "degraded signal. Reference is %.2f seconds. Degraded is %.2f seconds.",
      ref_duration, deg_duration);
  }

  // Error if the signals have different sample rates.
  if (ref_signal.sample_rate != deg_signal.sample_rate) {
    return google::protobuf::util::Status(
        google::protobuf::util::error::Code::INVALID_ARGUMENT,
        "Input audio signals have different sample rates! Reference audio "
        "sample rate: "+std::to_string(ref_signal.sample_rate)+". Degraded "
        "audio sample rate: "+std::to_string(deg_signal.sample_rate));
  }

  if (use_speech_mode_) {
    // Warn if input sample rate is > 16khz.
    if (ref_signal.sample_rate > k16kSampleRate) {
      ABSL_RAW_LOG(WARNING, "Input audio sample rate is above 16kHz, which"
                   " may have undesired effects for speech mode.  Consider"
                   " resampling to 16kHz.");
    }
  } else {
    // Warn if the signals' sample rate is not 48k for full audio mode.
    if (ref_signal.sample_rate != k48kSampleRate) {
      ABSL_RAW_LOG(WARNING, "Input audio does not have the expected sample"
                   " rate of 48kHz! This may negatively effect the prediction"
                   " of the MOS-LQO  score.");
    }
  }

  return google::protobuf::util::Status();
}
}  // namespace Visqol

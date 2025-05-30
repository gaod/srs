//
// Copyright (c) 2013-2025 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#include <srs_app_stream_bridge.hpp>

#include <srs_app_source.hpp>
#include <srs_protocol_format.hpp>
#include <srs_app_rtc_source.hpp>
#include <srs_app_config.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_kernel_rtc_rtp.hpp>
#include <srs_core_autofree.hpp>

#include <vector>
using namespace std;

ISrsStreamBridge::ISrsStreamBridge()
{
}

ISrsStreamBridge::~ISrsStreamBridge()
{
}

SrsFrameToRtmpBridge::SrsFrameToRtmpBridge(SrsSharedPtr<SrsLiveSource> source)
{
    source_ = source;
}

SrsFrameToRtmpBridge::~SrsFrameToRtmpBridge()
{
}

srs_error_t SrsFrameToRtmpBridge::initialize(SrsRequest* r)
{
    return srs_success;
}

srs_error_t SrsFrameToRtmpBridge::on_publish()
{
    srs_error_t err = srs_success;

    // TODO: FIXME: Should sync with bridge?
    if ((err = source_->on_publish()) != srs_success) {
        return srs_error_wrap(err, "source publish");
    }

    return err;
}

void SrsFrameToRtmpBridge::on_unpublish()
{
    // TODO: FIXME: Should sync with bridge?
    source_->on_unpublish();
}

srs_error_t SrsFrameToRtmpBridge::on_frame(SrsSharedPtrMessage* frame)
{
    return source_->on_frame(frame);
}

#ifdef SRS_RTC
SrsFrameToRtcBridge::SrsFrameToRtcBridge(SrsSharedPtr<SrsRtcSource> source)
{
    source_ = source;

#if defined(SRS_FFMPEG_FIT)
    uint32_t audio_ssrc = 0;
    uint8_t audio_payload_type = 0;
    uint32_t video_ssrc = 0;
    uint8_t video_payload_type = 0;

    // audio track ssrc
    if (true) {
        std::vector<SrsRtcTrackDescription*> descs = source->get_track_desc("audio", "opus");
        if (!descs.empty()) {
            audio_ssrc = descs.at(0)->ssrc_;
        }
        // Note we must use the PT of source, see https://github.com/ossrs/srs/pull/3079
        audio_payload_type = descs.empty() ? kAudioPayloadType : descs.front()->media_->pt_;
    }

    // video track ssrc
    if (true) {
        std::vector<SrsRtcTrackDescription*> descs = source->get_track_desc("video", "");
        if (!descs.empty()) {
            video_ssrc = descs.at(0)->ssrc_;
        }
        // Note we must use the PT of source, see https://github.com/ossrs/srs/pull/3079
        video_payload_type = descs.empty() ? kVideoPayloadType : descs.front()->media_->pt_;
    }

    rtp_builder_ = new SrsRtcRtpBuilder(this, audio_ssrc, audio_payload_type, video_ssrc, video_payload_type);
#endif

    video_codec_id_ = SrsVideoCodecIdReserved;
}

SrsFrameToRtcBridge::~SrsFrameToRtcBridge()
{
#ifdef SRS_FFMPEG_FIT
    srs_freep(rtp_builder_);
#endif
}

srs_error_t SrsFrameToRtcBridge::initialize(SrsRequest* r)
{
#ifdef SRS_FFMPEG_FIT
    return rtp_builder_->initialize(r);
#else
    return srs_success;
#endif
}

srs_error_t SrsFrameToRtcBridge::on_publish()
{
    srs_error_t err = srs_success;

    // TODO: FIXME: Should sync with bridge?
    if ((err = source_->on_publish()) != srs_success) {
        return srs_error_wrap(err, "source publish");
    }

#ifdef SRS_FFMPEG_FIT
    if ((err = rtp_builder_->on_publish()) != srs_success) {
        return srs_error_wrap(err, "rtp builder publish");
    }
#endif

    return err;
}

void SrsFrameToRtcBridge::on_unpublish()
{
#ifdef SRS_FFMPEG_FIT
    rtp_builder_->on_unpublish();
#endif

    // @remark This bridge might be disposed here, so never use it.
    // TODO: FIXME: Should sync with bridge?
    source_->on_unpublish();
}

srs_error_t SrsFrameToRtcBridge::on_frame(SrsSharedPtrMessage* frame)
{
#ifdef SRS_FFMPEG_FIT
    return rtp_builder_->on_frame(frame);
#else
    return srs_success;
#endif
}

srs_error_t SrsFrameToRtcBridge::on_rtp(SrsRtpPacket* pkt)
{
    return source_->on_rtp(pkt);
}

srs_error_t SrsFrameToRtcBridge::update_codec(SrsVideoCodecId id)
{
    srs_error_t err = srs_success;

    if (video_codec_id_ == id) {
        return err;
    }

    std::vector<SrsRtcTrackDescription*> video_track_descs = source_->get_track_desc("video", "");
    if (video_track_descs.empty()) {
        return srs_error_new(ERROR_RTC_NO_TRACK, "no track found for conversion");
    }

    SrsRtcTrackDescription* video_track_desc = video_track_descs.at(0);
    SrsVideoPayload* video_payload = (SrsVideoPayload*)video_track_desc->media_;
    
    if (id == SrsVideoCodecIdHEVC) {
        video_payload->name_ = "H265";
        video_payload->set_h265_param_desc("level-id=180;profile-id=1;tier-flag=0;tx-mode=SRST");
    } else {
        video_payload->name_ = "H264";
        video_payload->set_h264_param_desc("level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f");
    }

    srs_trace("RTC: Switch video codec %d(%s) to %d(%s)", video_codec_id_, srs_video_codec_id2str(video_codec_id_).c_str(),
            id, srs_video_codec_id2str(id).c_str());

    video_codec_id_ = id;

    return err;
}

#endif

SrsCompositeBridge::SrsCompositeBridge()
{
}

SrsCompositeBridge::~SrsCompositeBridge()
{
    for (vector<ISrsStreamBridge*>::iterator it = bridges_.begin(); it != bridges_.end(); ++it) {
        ISrsStreamBridge* bridge = *it;
        srs_freep(bridge);
    }
}

srs_error_t SrsCompositeBridge::initialize(SrsRequest* r)
{
    srs_error_t err = srs_success;

    for (vector<ISrsStreamBridge*>::iterator it = bridges_.begin(); it != bridges_.end(); ++it) {
        ISrsStreamBridge* bridge = *it;
        if ((err = bridge->initialize(r)) != srs_success) {
            return err;
        }
    }

    return err;
}

srs_error_t SrsCompositeBridge::on_publish()
{
    srs_error_t err = srs_success;

    for (vector<ISrsStreamBridge*>::iterator it = bridges_.begin(); it != bridges_.end(); ++it) {
        ISrsStreamBridge* bridge = *it;
        if ((err = bridge->on_publish()) != srs_success) {
            return err;
        }
    }

    return err;
}

void SrsCompositeBridge::on_unpublish()
{
    for (vector<ISrsStreamBridge*>::iterator it = bridges_.begin(); it != bridges_.end(); ++it) {
        ISrsStreamBridge* bridge = *it;
        bridge->on_unpublish();
    }
}

srs_error_t SrsCompositeBridge::on_frame(SrsSharedPtrMessage* frame)
{
    srs_error_t err = srs_success;

    for (vector<ISrsStreamBridge*>::iterator it = bridges_.begin(); it != bridges_.end(); ++it) {
        ISrsStreamBridge* bridge = *it;
        if ((err = bridge->on_frame(frame)) != srs_success) {
            return err;
        }
    }

    return err;
}

SrsCompositeBridge* SrsCompositeBridge::append(ISrsStreamBridge* bridge)
{
    bridges_.push_back(bridge);
    return this;
}


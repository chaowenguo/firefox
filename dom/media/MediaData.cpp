/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaData.h"

#include <functional>

#include "ImageContainer.h"
#include "MediaInfo.h"
#include "MediaResult.h"
#include "PerformanceRecorder.h"
#include "VideoUtils.h"
#include "YCbCrUtils.h"
#include "libyuv.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/layers/ImageBridgeChild.h"
#include "mozilla/layers/KnowsCompositor.h"
#include "mozilla/layers/SharedRGBImage.h"

#include <stdint.h>

#ifdef XP_WIN
#  include "mozilla/gfx/DeviceManagerDx.h"
#  include "mozilla/layers/D3D11ShareHandleImage.h"
#  include "mozilla/layers/D3D11YCbCrImage.h"
#elif XP_MACOSX
#  include "MacIOSurfaceImage.h"
#  include "mozilla/gfx/gfxVars.h"
#endif

namespace mozilla {

using namespace mozilla::gfx;
using layers::BufferRecycleBin;
using layers::PlanarYCbCrData;
using layers::PlanarYCbCrImage;
using media::TimeUnit;

const char* AudioData::sTypeName = "audio";
const char* VideoData::sTypeName = "video";

AudioData::AudioData(int64_t aOffset, const media::TimeUnit& aTime,
                     AlignedAudioBuffer&& aData, uint32_t aChannels,
                     uint32_t aRate, uint32_t aChannelMap)
    // Passing TimeUnit::Zero() here because we can't pass the result of an
    // arithmetic operation to the CheckedInt ctor. We set the duration in the
    // ctor body below.
    : MediaData(sType, aOffset, aTime, TimeUnit::Zero()),
      mChannels(aChannels),
      mChannelMap(aChannelMap),
      mRate(aRate),
      mOriginalTime(aTime),
      mAudioData(std::move(aData)),
      mFrames(mAudioData.Length() / aChannels) {
  MOZ_RELEASE_ASSERT(aChannels != 0,
                     "Can't create an AudioData with 0 channels.");
  MOZ_RELEASE_ASSERT(aRate != 0,
                     "Can't create an AudioData with a sample-rate of 0.");
  mDuration = TimeUnit(mFrames, aRate);
}

Span<AudioDataValue> AudioData::Data() const {
  return Span{GetAdjustedData(), mFrames * mChannels};
}

nsCString AudioData::ToString() const {
  nsCString rv;
  rv.AppendPrintf("AudioData: %s %s %" PRIu32 " frames %" PRIu32 "Hz, %" PRIu32
                  "ch",
                  mTime.ToString().get(), mDuration.ToString().get(), mFrames,
                  mRate, mChannels);
  return rv;
}

void AudioData::SetOriginalStartTime(const media::TimeUnit& aStartTime) {
  MOZ_ASSERT(mTime == mOriginalTime,
             "Do not call this if data has been trimmed!");
  mTime = aStartTime;
  mOriginalTime = aStartTime;
}

bool AudioData::AdjustForStartTime(const media::TimeUnit& aStartTime) {
  mOriginalTime -= aStartTime;
  mTime -= aStartTime;
  if (mTrimWindow) {
    *mTrimWindow -= aStartTime;
  }
  if (mTime.IsNegative()) {
    NS_WARNING("Negative audio start time after time-adjustment!");
  }
  return mTime.IsValid() && mOriginalTime.IsValid();
}

bool AudioData::SetTrimWindow(const media::TimeInterval& aTrim) {
  MOZ_DIAGNOSTIC_ASSERT(aTrim.mStart.IsValid() && aTrim.mEnd.IsValid(),
                        "An overflow occurred on the provided TimeInterval");
  if (!mAudioData) {
    // MoveableData got called. Can no longer work on it.
    return false;
  }
  if (aTrim.mStart < mOriginalTime || aTrim.mEnd > GetEndTime()) {
    return false;
  }

  auto trimBefore = aTrim.mStart - mOriginalTime;
  auto trimAfter = aTrim.mEnd - mOriginalTime;
  if (!trimBefore.IsValid() || !trimAfter.IsValid()) {
    // Overflow.
    return false;
  }
  if (!mTrimWindow && trimBefore.IsZero() && trimAfter == mDuration) {
    // Nothing to change, abort early to prevent rounding errors.
    return true;
  }

  size_t frameOffset = trimBefore.ToTicksAtRate(mRate);
  mTrimWindow = Some(aTrim);
  mDataOffset = frameOffset * mChannels;
  MOZ_DIAGNOSTIC_ASSERT(mDataOffset <= mAudioData.Length(),
                        "Data offset outside original buffer");
  int64_t frameCountAfterTrim = (trimAfter - trimBefore).ToTicksAtRate(mRate);
  if (frameCountAfterTrim >
      AssertedCast<int64_t>(mAudioData.Length() / mChannels)) {
    // Accept rounding error caused by an imprecise time_base in the container,
    // that can cause a mismatch but not other kind of unexpected frame count.
    MOZ_RELEASE_ASSERT(!trimBefore.IsBase(mRate));
    mFrames = 0;
  } else {
    mFrames = frameCountAfterTrim;
  }
  mTime = mOriginalTime + trimBefore;
  mDuration = TimeUnit(mFrames, mRate);

  return true;
}

AudioDataValue* AudioData::GetAdjustedData() const {
  if (!mAudioData) {
    return nullptr;
  }
  return mAudioData.Data() + mDataOffset;
}

void AudioData::EnsureAudioBuffer() {
  if (mAudioBuffer || !mAudioData) {
    return;
  }
  const AudioDataValue* srcData = GetAdjustedData();
  CheckedInt<size_t> bufferSize(sizeof(AudioDataValue));
  bufferSize *= mFrames;
  bufferSize *= mChannels;
  mAudioBuffer = SharedBuffer::Create(bufferSize);

  AudioDataValue* destData = static_cast<AudioDataValue*>(mAudioBuffer->Data());
  for (uint32_t i = 0; i < mFrames; ++i) {
    for (uint32_t j = 0; j < mChannels; ++j) {
      destData[j * mFrames + i] = srcData[i * mChannels + j];
    }
  }
}

size_t AudioData::SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const {
  size_t size =
      aMallocSizeOf(this) + mAudioData.SizeOfExcludingThis(aMallocSizeOf);
  if (mAudioBuffer) {
    size += mAudioBuffer->SizeOfIncludingThis(aMallocSizeOf);
  }
  return size;
}

AlignedAudioBuffer AudioData::MoveableData() {
  // Trim buffer according to trimming mask.
  mAudioData.PopFront(mDataOffset);
  mAudioData.SetLength(mFrames * mChannels);
  mDataOffset = 0;
  mFrames = 0;
  mTrimWindow.reset();
  return std::move(mAudioData);
}

static bool ValidatePlane(const VideoData::YCbCrBuffer::Plane& aPlane) {
  return aPlane.mWidth <= PlanarYCbCrImage::MAX_DIMENSION &&
         aPlane.mHeight <= PlanarYCbCrImage::MAX_DIMENSION &&
         aPlane.mWidth * aPlane.mHeight < MAX_VIDEO_WIDTH * MAX_VIDEO_HEIGHT &&
         aPlane.mStride > 0 && aPlane.mWidth <= aPlane.mStride;
}

static MediaResult ValidateBufferAndPicture(
    const VideoData::YCbCrBuffer& aBuffer, const IntRect& aPicture) {
  // The following situation should never happen unless there is a bug
  // in the decoder
  if (aBuffer.mPlanes[1].mWidth != aBuffer.mPlanes[2].mWidth ||
      aBuffer.mPlanes[1].mHeight != aBuffer.mPlanes[2].mHeight) {
    return MediaResult(NS_ERROR_INVALID_ARG,
                       "Chroma planes with different sizes");
  }

  // The following situations could be triggered by invalid input
  if (aPicture.width <= 0 || aPicture.height <= 0) {
    return MediaResult(NS_ERROR_INVALID_ARG, "Empty picture rect");
  }
  if (!ValidatePlane(aBuffer.mPlanes[0]) ||
      !ValidatePlane(aBuffer.mPlanes[1]) ||
      !ValidatePlane(aBuffer.mPlanes[2])) {
    return MediaResult(NS_ERROR_INVALID_ARG, "Invalid plane size");
  }

  // Ensure the picture size specified in the headers can be extracted out of
  // the frame we've been supplied without indexing out of bounds.
  CheckedUint32 xLimit = aPicture.x + CheckedUint32(aPicture.width);
  CheckedUint32 yLimit = aPicture.y + CheckedUint32(aPicture.height);
  if (!xLimit.isValid() || xLimit.value() > aBuffer.mPlanes[0].mStride ||
      !yLimit.isValid() || yLimit.value() > aBuffer.mPlanes[0].mHeight) {
    // The specified picture dimensions can't be contained inside the video
    // frame, we'll stomp memory if we try to copy it. Fail.
    return MediaResult(NS_ERROR_INVALID_ARG, "Overflowing picture rect");
  }
  return MediaResult(NS_OK);
}

VideoData::VideoData(int64_t aOffset, const TimeUnit& aTime,
                     const TimeUnit& aDuration, bool aKeyframe,
                     const TimeUnit& aTimecode, IntSize aDisplay,
                     layers::ImageContainer::FrameID aFrameID)
    : MediaData(Type::VIDEO_DATA, aOffset, aTime, aDuration),
      mDisplay(aDisplay),
      mFrameID(aFrameID),
      mSentToCompositor(false),
      mNextKeyFrameTime(TimeUnit::Invalid()) {
  MOZ_ASSERT(!mDuration.IsNegative(), "Frame must have non-negative duration.");
  mKeyframe = aKeyframe;
  mTimecode = aTimecode;
}

VideoData::~VideoData() = default;

size_t VideoData::SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const {
  size_t size = aMallocSizeOf(this);

  // Currently only PLANAR_YCBCR has a well defined function for determining
  // it's size, so reporting is limited to that type.
  if (mImage && mImage->GetFormat() == ImageFormat::PLANAR_YCBCR) {
    const mozilla::layers::PlanarYCbCrImage* img =
        static_cast<const mozilla::layers::PlanarYCbCrImage*>(mImage.get());
    size += img->SizeOfIncludingThis(aMallocSizeOf);
  }

  return size;
}

ColorDepth VideoData::GetColorDepth() const {
  if (!mImage) {
    return ColorDepth::COLOR_8;
  }

  return mImage->GetColorDepth();
}

void VideoData::UpdateDuration(const TimeUnit& aDuration) {
  MOZ_ASSERT(!aDuration.IsNegative());
  mDuration = aDuration;
}

void VideoData::UpdateTimestamp(const TimeUnit& aTimestamp) {
  MOZ_ASSERT(!aTimestamp.IsNegative());

  auto updatedDuration = GetEndTime() - aTimestamp;
  MOZ_ASSERT(!updatedDuration.IsNegative());

  mTime = aTimestamp;
  mDuration = updatedDuration;
}

bool VideoData::AdjustForStartTime(const media::TimeUnit& aStartTime) {
  mTime -= aStartTime;
  if (mTime.IsNegative()) {
    NS_WARNING("Negative video start time after time-adjustment!");
  }
  return mTime.IsValid();
}

PlanarYCbCrData ConstructPlanarYCbCrData(const VideoInfo& aInfo,
                                         const VideoData::YCbCrBuffer& aBuffer,
                                         const IntRect& aPicture) {
  const VideoData::YCbCrBuffer::Plane& Y = aBuffer.mPlanes[0];
  const VideoData::YCbCrBuffer::Plane& Cb = aBuffer.mPlanes[1];
  const VideoData::YCbCrBuffer::Plane& Cr = aBuffer.mPlanes[2];

  PlanarYCbCrData data;
  data.mYChannel = Y.mData;
  data.mYStride = AssertedCast<int32_t>(Y.mStride);
  data.mYSkip = AssertedCast<int32_t>(Y.mSkip);
  data.mCbChannel = Cb.mData;
  data.mCrChannel = Cr.mData;
  data.mCbCrStride = AssertedCast<int32_t>(Cb.mStride);
  data.mCbSkip = AssertedCast<int32_t>(Cb.mSkip);
  data.mCrSkip = AssertedCast<int32_t>(Cr.mSkip);
  data.mPictureRect = aPicture;
  data.mStereoMode = aInfo.mStereoMode;
  data.mYUVColorSpace = aBuffer.mYUVColorSpace;
  data.mColorPrimaries = aBuffer.mColorPrimaries;
  data.mColorDepth = aBuffer.mColorDepth;
  if (aInfo.mTransferFunction) {
    data.mTransferFunction = *aInfo.mTransferFunction;
  }
  data.mColorRange = aBuffer.mColorRange;
  data.mChromaSubsampling = aBuffer.mChromaSubsampling;
  return data;
}

/* static */
MediaResult VideoData::SetVideoDataToImage(PlanarYCbCrImage* aVideoImage,
                                           const VideoInfo& aInfo,
                                           const YCbCrBuffer& aBuffer,
                                           const IntRect& aPicture,
                                           bool aCopyData) {
  MOZ_ASSERT(aVideoImage);

  PlanarYCbCrData data = ConstructPlanarYCbCrData(aInfo, aBuffer, aPicture);

  if (aCopyData) {
    return MediaResult(aVideoImage->CopyData(data),
                       RESULT_DETAIL("Failed to copy image data"));
  }
  return MediaResult(aVideoImage->AdoptData(data),
                     RESULT_DETAIL("Failed to adopt image data"));
}

/* static */
Result<already_AddRefed<VideoData>, MediaResult> VideoData::CreateAndCopyData(
    const VideoInfo& aInfo, ImageContainer* aContainer, int64_t aOffset,
    const TimeUnit& aTime, const TimeUnit& aDuration,
    const YCbCrBuffer& aBuffer, bool aKeyframe, const TimeUnit& aTimecode,
    const IntRect& aPicture, layers::KnowsCompositor* aAllocator) {
  if (!aContainer) {
    // Create a dummy VideoData with no image. This gives us something to
    // send to media streams if necessary.
    RefPtr<VideoData> v(new VideoData(aOffset, aTime, aDuration, aKeyframe,
                                      aTimecode, aInfo.mDisplay, 0));
    return v.forget();
  }

  if (MediaResult r = ValidateBufferAndPicture(aBuffer, aPicture);
      NS_FAILED(r)) {
    return Err(r);
  }

  PerformanceRecorder<PlaybackStage> perfRecorder(MediaStage::CopyDecodedVideo,
                                                  aInfo.mImage.height);
  RefPtr<VideoData> v(new VideoData(aOffset, aTime, aDuration, aKeyframe,
                                    aTimecode, aInfo.mDisplay, 0));

  // Currently our decoder only knows how to output to ImageFormat::PLANAR_YCBCR
  // format.
#if XP_MACOSX
  if (aAllocator && aAllocator->GetWebRenderCompositorType() !=
                        layers::WebRenderCompositor::SOFTWARE) {
    RefPtr<layers::MacIOSurfaceImage> ioImage =
        new layers::MacIOSurfaceImage(nullptr);
    PlanarYCbCrData data = ConstructPlanarYCbCrData(aInfo, aBuffer, aPicture);
    if (ioImage->SetData(aContainer, data)) {
      v->mImage = ioImage;
      perfRecorder.Record();
      return v.forget();
    }
  }
#endif
  if (!v->mImage) {
    v->mImage = aContainer->CreatePlanarYCbCrImage();
  }

  if (!v->mImage) {
    // TODO: Should other error like NS_ERROR_UNEXPECTED be used here to
    // distinguish this error from the NS_ERROR_OUT_OF_MEMORY below?
    return Err(MediaResult(NS_ERROR_OUT_OF_MEMORY,
                           "Failed to create a PlanarYCbCrImage"));
  }
  NS_ASSERTION(v->mImage->GetFormat() == ImageFormat::PLANAR_YCBCR,
               "Wrong format?");
  PlanarYCbCrImage* videoImage = v->mImage->AsPlanarYCbCrImage();
  MOZ_ASSERT(videoImage);
  videoImage->SetColorDepth(aBuffer.mColorDepth);

  if (MediaResult r = VideoData::SetVideoDataToImage(
          videoImage, aInfo, aBuffer, aPicture, true /* aCopyData */);
      NS_FAILED(r)) {
    return Err(r);
  }

  perfRecorder.Record();
  return v.forget();
}

/* static */
already_AddRefed<VideoData> VideoData::CreateAndCopyData(
    const VideoInfo& aInfo, ImageContainer* aContainer, int64_t aOffset,
    const TimeUnit& aTime, const TimeUnit& aDuration,
    const YCbCrBuffer& aBuffer, const YCbCrBuffer::Plane& aAlphaPlane,
    bool aKeyframe, const TimeUnit& aTimecode, const IntRect& aPicture) {
  if (!aContainer) {
    // Create a dummy VideoData with no image. This gives us something to
    // send to media streams if necessary.
    RefPtr<VideoData> v(new VideoData(aOffset, aTime, aDuration, aKeyframe,
                                      aTimecode, aInfo.mDisplay, 0));
    return v.forget();
  }

  if (MediaResult r = ValidateBufferAndPicture(aBuffer, aPicture);
      NS_FAILED(r)) {
    NS_ERROR(r.Message().get());
    return nullptr;
  }

  RefPtr<VideoData> v(new VideoData(aOffset, aTime, aDuration, aKeyframe,
                                    aTimecode, aInfo.mDisplay, 0));

  // Convert from YUVA to BGRA format on the software side.
  RefPtr<layers::SharedRGBImage> videoImage =
      aContainer->CreateSharedRGBImage();
  v->mImage = videoImage;

  if (!v->mImage) {
    return nullptr;
  }
  if (!videoImage->Allocate(
          IntSize(aBuffer.mPlanes[0].mWidth, aBuffer.mPlanes[0].mHeight),
          SurfaceFormat::B8G8R8A8)) {
    return nullptr;
  }

  RefPtr<layers::TextureClient> texture =
      videoImage->GetTextureClient(/* aKnowsCompositor */ nullptr);
  if (!texture) {
    NS_WARNING("Failed to allocate TextureClient");
    return nullptr;
  }

  layers::TextureClientAutoLock autoLock(texture,
                                         layers::OpenMode::OPEN_WRITE_ONLY);
  if (!autoLock.Succeeded()) {
    NS_WARNING("Failed to lock TextureClient");
    return nullptr;
  }

  layers::MappedTextureData buffer;
  if (!texture->BorrowMappedData(buffer)) {
    NS_WARNING("Failed to borrow mapped data");
    return nullptr;
  }

  // The naming convention for libyuv and associated utils is word-order.
  // The naming convention in the gfx stack is byte-order.
  nsresult result = ConvertI420AlphaToARGB(
      aBuffer.mPlanes[0].mData, aBuffer.mPlanes[1].mData,
      aBuffer.mPlanes[2].mData, aAlphaPlane.mData,
      AssertedCast<int>(aBuffer.mPlanes[0].mStride),
      AssertedCast<int>(aBuffer.mPlanes[1].mStride), buffer.data, buffer.stride,
      buffer.size.width, buffer.size.height);
  if (NS_FAILED(result)) {
    MOZ_ASSERT_UNREACHABLE("Failed to convert I420 YUVA into RGBA data");
    return nullptr;
  }

  return v.forget();
}

/* static */
already_AddRefed<VideoData> VideoData::CreateFromImage(
    const IntSize& aDisplay, int64_t aOffset, const TimeUnit& aTime,
    const TimeUnit& aDuration, const RefPtr<Image>& aImage, bool aKeyframe,
    const TimeUnit& aTimecode) {
  RefPtr<VideoData> v(new VideoData(aOffset, aTime, aDuration, aKeyframe,
                                    aTimecode, aDisplay, 0));
  v->mImage = aImage;
  return v.forget();
}

nsCString VideoData::ToString() const {
  std::array ImageFormatStrings = {
      "PLANAR_YCBCR",
      "NV_IMAGE",
      "SHARED_RGB",
      "MOZ2D_SURFACE",
      "MAC_IOSURFACE",
      "SURFACE_TEXTURE",
      "D3D9_RGB32_TEXTURE",
      "OVERLAY_IMAGE",
      "D3D11_SHARE_HANDLE_TEXTURE",
      "D3D11_TEXTURE_ZERO_COPY",
      "TEXTURE_WRAPPER",
      "GPU_VIDEO",
      "DMABUF",
      "DCOMP_SURFACE",
  };

  nsCString rv;
  rv.AppendPrintf(
      "VideoFrame [%s,%s] [%dx%d] format: %s", mTime.ToString().get(),
      mDuration.ToString().get(), mDisplay.Width(), mDisplay.Height(),
      mImage ? ImageFormatStrings[static_cast<int>(mImage->GetFormat())]
             : "null");
  return rv;
}

MediaResult VideoData::QuantizableBuffer::To8BitPerChannel(
    BufferRecycleBin* aRecycleBin) {
  MOZ_ASSERT(!mRecycleBin, "Should not be called more than once.");
  mRecycleBin = aRecycleBin;

  MOZ_ASSERT(mColorDepth == ColorDepth::COLOR_10 ||
             mColorDepth == ColorDepth::COLOR_12);
  int yStride = mPlanes[0].mStride / 2;
  int uvStride = mPlanes[1].mStride / 2;
  size_t yLength = yStride * mPlanes[0].mHeight;
  size_t uvLength = uvStride * mPlanes[1].mHeight;

  const uint16_t* srcPlanes[3]{
      reinterpret_cast<const uint16_t*>(mPlanes[0].mData),
      reinterpret_cast<const uint16_t*>(mPlanes[1].mData),
      reinterpret_cast<const uint16_t*>(mPlanes[2].mData)};
  AllocateRecyclableData(yLength + (uvLength * 2));
  if (!m8bpcPlanes) {
    return MediaResult(
        NS_ERROR_OUT_OF_MEMORY,
        RESULT_DETAIL("Cannot allocate %zu bytes for 8-bit conversion",
                      yLength + (uvLength * 2)));
  }
  uint8_t* destPlanes[3]{m8bpcPlanes.get(), m8bpcPlanes.get() + yLength,
                         m8bpcPlanes.get() + yLength + uvLength};
  using Func16To8 =  // libyuv function type.
      std::function<int(const uint16_t*, int, const uint16_t*, int,
                        const uint16_t*, int, uint8_t*, int, uint8_t*, int,
                        uint8_t*, int, int, int)>;
  auto convertFunc = [](ColorDepth aDepth,
                        ChromaSubsampling aSubsampling) -> Func16To8 {
    switch (aSubsampling) {
      case ChromaSubsampling::HALF_WIDTH_AND_HEIGHT:  // 420p
        return aDepth == ColorDepth::COLOR_10 ? libyuv::I010ToI420
                                              : libyuv::I012ToI420;
      case ChromaSubsampling::HALF_WIDTH:  // 422p
        return aDepth == ColorDepth::COLOR_10 ? libyuv::I210ToI422
                                              : libyuv::I212ToI422;
      case ChromaSubsampling::FULL:  // 444p
        return aDepth == ColorDepth::COLOR_10 ? libyuv::I410ToI444
                                              : libyuv::I412ToI444;
      default:
        return Func16To8();
    }
  }(mColorDepth, mChromaSubsampling);
  if (!convertFunc) {
    return MediaResult(
        NS_ERROR_DOM_MEDIA_DECODE_ERR,
        RESULT_DETAIL("Source format (color depth=%d, subsampling=%" PRIu8
                      ") not supported",
                      BitDepthForColorDepth(mColorDepth),
                      static_cast<uint8_t>(mChromaSubsampling)));
  }
  int r = convertFunc(srcPlanes[0], yStride, srcPlanes[1], uvStride,
                      srcPlanes[2], uvStride, destPlanes[0], yStride,
                      destPlanes[1], uvStride, destPlanes[2], uvStride,
                      mPlanes[0].mWidth, mPlanes[0].mHeight);
  if (r != 0) {
    return MediaResult(
        NS_ERROR_DOM_MEDIA_DECODE_ERR,
        RESULT_DETAIL("Conversion to 8-bit failed. libyuv error=%d", r));
  }
  // Update buffer info.
  mColorDepth = ColorDepth::COLOR_8;
  mPlanes[0].mData = destPlanes[0];
  mPlanes[0].mStride = yStride;
  mPlanes[1].mData = destPlanes[1];
  mPlanes[2].mData = destPlanes[2];
  mPlanes[1].mStride = mPlanes[2].mStride = uvStride;

  return MediaResult(NS_OK);
}

void VideoData::QuantizableBuffer::AllocateRecyclableData(size_t aLength) {
  MOZ_ASSERT(!m8bpcPlanes, "Should not allocate more than once.");
  MOZ_ASSERT(aLength > 0, "Zero-length allocation!");

  m8bpcPlanes = mRecycleBin->GetBuffer(aLength);
  mAllocatedLength = aLength;
}

VideoData::QuantizableBuffer::~QuantizableBuffer() {
  if (m8bpcPlanes) {
    mRecycleBin->RecycleBuffer(std::move(m8bpcPlanes), mAllocatedLength);
  }
}

MediaRawData::MediaRawData()
    : MediaData(Type::RAW_DATA), mCrypto(mCryptoInternal) {}

MediaRawData::MediaRawData(const uint8_t* aData, size_t aSize)
    : MediaData(Type::RAW_DATA),
      mCrypto(mCryptoInternal),
      mBuffer(aData, aSize) {}

MediaRawData::MediaRawData(const uint8_t* aData, size_t aSize,
                           const uint8_t* aAlphaData, size_t aAlphaSize)
    : MediaData(Type::RAW_DATA),
      mCrypto(mCryptoInternal),
      mBuffer(aData, aSize),
      mAlphaBuffer(aAlphaData, aAlphaSize) {}

MediaRawData::MediaRawData(AlignedByteBuffer&& aData)
    : MediaData(Type::RAW_DATA),
      mCrypto(mCryptoInternal),
      mBuffer(std::move(aData)) {}

MediaRawData::MediaRawData(AlignedByteBuffer&& aData,
                           AlignedByteBuffer&& aAlphaData)
    : MediaData(Type::RAW_DATA),
      mCrypto(mCryptoInternal),
      mBuffer(std::move(aData)),
      mAlphaBuffer(std::move(aAlphaData)) {}

already_AddRefed<MediaRawData> MediaRawData::Clone() const {
  int32_t sampleHeight = 0;
  if (mTrackInfo && mTrackInfo->GetAsVideoInfo()) {
    sampleHeight = mTrackInfo->GetAsVideoInfo()->mImage.height;
  }
  PerformanceRecorder<PlaybackStage> perfRecorder(MediaStage::CopyDemuxedData,
                                                  sampleHeight);
  RefPtr<MediaRawData> s = new MediaRawData;
  s->mTimecode = mTimecode;
  s->mTime = mTime;
  s->mDuration = mDuration;
  s->mOffset = mOffset;
  s->mKeyframe = mKeyframe;
  s->mExtraData = mExtraData;
  s->mCryptoInternal = mCryptoInternal;
  s->mTrackInfo = mTrackInfo;
  s->mEOS = mEOS;
  s->mOriginalPresentationWindow = mOriginalPresentationWindow;
  if (!s->mBuffer.Append(mBuffer.Data(), mBuffer.Length())) {
    return nullptr;
  }
  if (!s->mAlphaBuffer.Append(mAlphaBuffer.Data(), mAlphaBuffer.Length())) {
    return nullptr;
  }
  perfRecorder.Record();
  return s.forget();
}

MediaRawData::~MediaRawData() = default;

size_t MediaRawData::SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const {
  size_t size = aMallocSizeOf(this);
  size += mBuffer.SizeOfExcludingThis(aMallocSizeOf);
  return size;
}

UniquePtr<MediaRawDataWriter> MediaRawData::CreateWriter() {
  UniquePtr<MediaRawDataWriter> p(new MediaRawDataWriter(this));
  return p;
}

MediaRawDataWriter::MediaRawDataWriter(MediaRawData* aMediaRawData)
    : mCrypto(aMediaRawData->mCryptoInternal), mTarget(aMediaRawData) {}

bool MediaRawDataWriter::SetSize(size_t aSize) {
  return mTarget->mBuffer.SetLength(aSize);
}

bool MediaRawDataWriter::Prepend(const uint8_t* aData, size_t aSize) {
  return mTarget->mBuffer.Prepend(aData, aSize);
}

bool MediaRawDataWriter::Append(const uint8_t* aData, size_t aSize) {
  return mTarget->mBuffer.Append(aData, aSize);
}

bool MediaRawDataWriter::Replace(const uint8_t* aData, size_t aSize) {
  return mTarget->mBuffer.Replace(aData, aSize);
}

void MediaRawDataWriter::Clear() { mTarget->mBuffer.Clear(); }

uint8_t* MediaRawDataWriter::Data() { return mTarget->mBuffer.Data(); }

size_t MediaRawDataWriter::Size() { return mTarget->Size(); }

void MediaRawDataWriter::PopFront(size_t aSize) {
  mTarget->mBuffer.PopFront(aSize);
}

nsCString CryptoSchemeSetToString(const CryptoSchemeSet& aSchemes) {
  nsAutoCString rv;
  if (aSchemes.contains(CryptoScheme::Cenc)) {
    rv.AppendLiteral("cenc");
  }
  if (aSchemes.contains(CryptoScheme::Cbcs)) {
    if (!rv.IsEmpty()) {
      rv.AppendLiteral("/");
    }
    rv.AppendLiteral("cbcs");
  }
  if (aSchemes.contains(CryptoScheme::Cbcs_1_9)) {
    if (!rv.IsEmpty()) {
      rv.AppendLiteral("/");
    }
    rv.AppendLiteral("cbcs-1-9");
  }
  if (rv.IsEmpty()) {
    rv.AppendLiteral("none");
  }
  return std::move(rv);
}

CryptoScheme StringToCryptoScheme(const nsAString& aString) {
  if (aString.EqualsLiteral("cenc")) {
    return CryptoScheme::Cenc;
  }
  if (aString.EqualsLiteral("cbcs")) {
    return CryptoScheme::Cbcs;
  }
  if (aString.EqualsLiteral("cbcs-1-9")) {
    return CryptoScheme::Cbcs_1_9;
  }
  return CryptoScheme::None;
}

}  // namespace mozilla

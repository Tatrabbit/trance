#include "image.h"
#include <iostream>
#include <gif_lib.h>
#include <jpgd.h>
#include <mkvreader.hpp>
#include <mkvparser.hpp>
#include <SFML/OpenGL.hpp>
#include "util.h"

#define VPX_CODEC_DISABLE_COMPAT 1
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>
#include <vpx/vpx_image.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>

namespace {

std::vector<Image> load_animation_gif(const std::string& path)
{
  int error_code = 0;
  GifFileType* gif = DGifOpenFileName(path.c_str(), &error_code);
  if (!gif) {
    std::cerr << "couldn't load " << path <<
        ": " << GifErrorString(error_code) << std::endl;
    return {};
  }
  if (DGifSlurp(gif) != GIF_OK) {
    std::cerr << "couldn't slurp " << path <<
        ": " << GifErrorString(gif->Error) << std::endl;
    if (DGifCloseFile(gif, &error_code) != GIF_OK) {
      std::cerr << "couldn't close " << path <<
          ": " << GifErrorString(error_code) << std::endl;
    }
    return {};
  }

  auto width = gif->SWidth;
  auto height = gif->SHeight;
  std::unique_ptr<uint32_t[]> pixels(new uint32_t[width * height]);
  for (int i = 0; i < width * height; ++i) {
    pixels[i] = gif->SBackGroundColor;
  }

  std::vector<Image> result;
  for (int i = 0; i < gif->ImageCount; ++i) {
    const auto& frame = gif->SavedImages[i];
    bool transparency = false;
    uint8_t transparency_byte = 0;
    // Delay time in hundredths of a second. Ignore it; it messes with the
    // rhythm.
    int delay_time = 1;
    for (int j = 0; j < frame.ExtensionBlockCount; ++j) {
      const auto& block = frame.ExtensionBlocks[j];
      if (block.Function != GRAPHICS_EXT_FUNC_CODE) {
        continue;
      }

      char dispose = (block.Bytes[0] >> 2) & 7;
      transparency = block.Bytes[0] & 1;
      delay_time = block.Bytes[1] + (block.Bytes[2] << 8);
      transparency_byte = block.Bytes[3];

      if (dispose == 2) {
        for (int k = 0; k < width * height; ++k) {
          pixels[k] = gif->SBackGroundColor;
        }
      }
    }
    auto map = frame.ImageDesc.ColorMap ?
        frame.ImageDesc.ColorMap : gif->SColorMap;

    auto fw = frame.ImageDesc.Width;
    auto fh = frame.ImageDesc.Height;
    auto fl = frame.ImageDesc.Left;
    auto ft = frame.ImageDesc.Top;

    for (int y = 0; y < std::min(height, fh); ++y) {
      for (int x = 0; x < std::min(width, fw); ++x) {
        uint8_t byte = frame.RasterBits[x + y * fw];
        if (transparency && byte == transparency_byte) {
          continue;
        }
        const auto& c = map->Colors[byte];
        // Still get segfaults here sometimes...
        pixels[fl + x + (ft + y) * width] =
            c.Red | (c.Green << 8) | (c.Blue << 16) | (0xff << 24);
      }
    }

    result.emplace_back(width, height, (unsigned char*) pixels.get());
    std::cout << ";";
  }

  if (DGifCloseFile(gif, &error_code) != GIF_OK) {
    std::cerr << "couldn't close " << path <<
        ": " << GifErrorString(error_code) << std::endl;
  }
  return result;
}

std::vector<Image> load_animation_webm(const std::string& path)
{
  mkvparser::MkvReader reader;
  if (reader.Open(path.c_str())) {
    std::cerr << "couldn't open " << path << std::endl;
    return {};
  }

  long long pos = 0;
  mkvparser::EBMLHeader ebmlHeader;
  ebmlHeader.Parse(&reader, pos);

  mkvparser::Segment* segment_tmp;
  if (mkvparser::Segment::CreateInstance(&reader, pos, segment_tmp)) {
    std::cerr << "couldn't load " << path <<
        ": segment create failed" << std::endl;
    return {};
  }

  std::unique_ptr<mkvparser::Segment> segment(segment_tmp);
  if (segment->Load() < 0) {
    std::cerr << "couldn't load " << path <<
        ": segment load failed" << std::endl;
    return {};
  }

  const mkvparser::VideoTrack* video_track = nullptr;
  for (unsigned long i = 0; i < segment->GetTracks()->GetTracksCount(); ++i) {
    const auto& track = segment->GetTracks()->GetTrackByIndex(i);
    if (track && track->GetType() && mkvparser::Track::kVideo ||
        track->GetCodecNameAsUTF8() == std::string("VP8")) {
      video_track = (const mkvparser::VideoTrack*) track;
      break;
    }
  }

  if (!video_track) {
    std::cerr << "couldn't load " << path <<
        ": no VP8 video track found" << std::endl;
    return {};
  }

  vpx_codec_ctx_t codec;
  auto codec_error = [&](const std::string& s) {
    auto detail = vpx_codec_error_detail(&codec);
    std::cerr << "couldn't load " << path <<
        ": " << s << ": " << vpx_codec_error(&codec);
    if (detail) {
      std::cerr << ": " << detail;
    }
    std::cerr << std::endl;
  };

  if (vpx_codec_dec_init(&codec, vpx_codec_vp8_dx(), nullptr, 0)) {
    codec_error("initialising codec");
    return {};
  }

  std::vector<Image> result;
  for (auto cluster = segment->GetFirst(); cluster && !cluster->EOS();
       cluster = segment->GetNext(cluster)) {
    const mkvparser::BlockEntry* block;
    if (cluster->GetFirst(block) < 0) {
      std::cerr << "couldn't load " << path <<
          ": couldn't parse first block of cluster" << std::endl;
      return {};
    }

    while (block && !block->EOS()) {
      const auto& b = block->GetBlock();
      if (b->GetTrackNumber() == video_track->GetNumber()) {
        for (int i = 0; i < b->GetFrameCount(); ++i) {
          const auto& frame = b->GetFrame(i);
          std::unique_ptr<uint8_t[]> data(new uint8_t[frame.len]);
          reader.Read(frame.pos, frame.len, data.get());

          if (vpx_codec_decode(&codec, data.get(), frame.len, nullptr, 0)) {
            codec_error("decoding frame");
            return {};
          }

          vpx_codec_iter_t it = nullptr;
          while (auto img = vpx_codec_get_frame(&codec, &it)) {
            // Convert I420 (YUV with NxN Y-plane and (N/2)x(N/2) U- and V-
            // planes) to RGB.
            std::unique_ptr<uint32_t[]> data(new uint32_t[img->d_w * img->d_h]);
            auto w = img->d_w;
            auto h = img->d_h;
            for (uint32_t y = 0; y < h; ++y) {
              for (uint32_t x = 0; x < w; ++x) {
                uint8_t Y = img->planes[VPX_PLANE_Y]
                    [x + y * img->stride[VPX_PLANE_Y]];
                uint8_t U = img->planes[VPX_PLANE_U]
                    [x / 2 + (y / 2) * img->stride[VPX_PLANE_U]];
                uint8_t V = img->planes[VPX_PLANE_V]
                    [x / 2 + (y / 2) * img->stride[VPX_PLANE_V]];

                auto cl = [](float f) {
                  return (uint8_t) std::max(0, std::min(255, (int) f));
                };
                auto R = cl(1.164f * (Y - 16.f) + 1.596f * (V - 128.f));
                auto G = cl(1.164f * (Y - 16.f) -
                    0.391f * (U - 128.f) - 0.813f * (V - 128.f));
                auto B = cl(1.164f * (Y - 16.f) + 2.017f * (U - 128.f));
                data[x + y * w] =
                    R | (G << 8) | (B << 16) | (0xff << 24);
              }
            }

            result.emplace_back(w, h, (uint8_t*) data.get());
            std::cout << ";";
          }
        }
      }

      if (cluster->GetNext(block, block) < 0) {
        std::cerr << "couldn't load " << path <<
            ": couldn't parse next block of cluster" << std::endl;
        return {};
      }
    }
  }

  if (vpx_codec_destroy(&codec)) {
    codec_error("destroying codec");
    return {};
  }

  return result;
}

}

std::vector<GLuint> Image::textures_to_delete;
std::mutex Image::textures_to_delete_mutex;

Image::Image()
: _width{0}
, _height{0}
, _texture{0}
{
}

Image::Image(uint32_t width, uint32_t height, unsigned char* data)
: _width{width}
, _height{height}
, _texture{0}
, _sf_image{new std::shared_ptr<sf::Image>{new sf::Image}}
{
  (*_sf_image)->create(width, height, data);
}

Image::Image(const sf::Image& image)
: _width{image.getSize().x}
, _height{image.getSize().y}
, _texture{0}
, _sf_image{new std::shared_ptr<sf::Image>{new sf::Image{image}}}
{
}

Image::operator bool() const
{
  return _width && _height;
}

uint32_t Image::width() const
{
  return _width;
}

uint32_t Image::height() const
{
  return _height;
}

uint32_t Image::texture() const
{
  return _texture;
}

bool Image::ensure_texture_uploaded() const
{
  if (_texture || !(*this)) {
    return false;
  }

  // Upload the texture to video memory and set its texture_deleter so that
  // it's cleaned up when there are no more Image objects referencing it.
  glGenTextures(1, &_texture);
  _deleter.reset(new texture_deleter{_texture});

  // Could be split out to a separate call so that Theme doesn't have to hold on
  // to mutex while uploading. This probably doesn't actually block though so no
  // worries.
  glBindTexture(GL_TEXTURE_2D, _texture);
  glTexImage2D(
      GL_TEXTURE_2D, 0, GL_RGBA, _width, _height,
      0, GL_RGBA, GL_UNSIGNED_BYTE, (*_sf_image)->getPixelsPtr());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

  // Return true for purging on the async thread.
  std::cout << ":";
  return true;
}

Image::sf_image_ptr& Image::get_sf_image() const
{
  return _sf_image;
}

void Image::delete_textures()
{
  textures_to_delete_mutex.lock();
  for (const auto& texture : textures_to_delete) {
    glDeleteTextures(1, &texture);
  }
  textures_to_delete.clear();
  textures_to_delete_mutex.unlock();
}

Image::texture_deleter::~texture_deleter()
{
  textures_to_delete_mutex.lock();
  textures_to_delete.push_back(texture);
  textures_to_delete_mutex.unlock();
}

bool is_gif_animated(const std::string& path)
{
  int error_code = 0;
  GifFileType* gif = DGifOpenFileName(path.c_str(), &error_code);
  if (!gif) {
    std::cerr << "couldn't load " << path <<
        ": " << GifErrorString(error_code) << std::endl;
    return false;
  }
  int frames = 0;
  if (DGifSlurp(gif) != GIF_OK) {
    std::cerr << "couldn't slurp " << path <<
        ": " << GifErrorString(gif->Error) << std::endl;
  }
  else {
    frames = gif->ImageCount;
  }
  if (DGifCloseFile(gif, &error_code) != GIF_OK) {
    std::cerr << "couldn't close " << path <<
        ": " << GifErrorString(error_code) << std::endl;
  }
  return frames > 0;
}

Image load_image(const std::string& path)
{
  // Load JPEGs with the jpgd library since SFML does not support progressive
  // JPEGs.
  if (ext_is(path, "jpg") || ext_is(path, "jpeg")) {
    int width = 0;
    int height = 0;
    int reqs = 0;
    unsigned char* data = jpgd::decompress_jpeg_image_from_file(
        path.c_str(), &width, &height, &reqs, 4);
    if (!data) {
      std::cerr << "\ncouldn't load " << path << std::endl;
      return {};
    }

    Image image{uint32_t(width), uint32_t(height), data};
    free(data);
    std::cout << ".";
    return image;
  }

  sf::Image sf_image;
  if (!sf_image.loadFromFile(path)) {
    std::cerr << "\ncouldn't load " << path << std::endl;
    return {};
  }

  Image image{sf_image};
  std::cout << ".";
  return image;
}

std::vector<Image> load_animation(const std::string& path)
{
  if (ext_is(path, "gif")) {
    return load_animation_gif(path);
  }
  if (ext_is(path, "webm")) {
    return load_animation_webm(path);
  }
  return {};
}

FrameExporter::FrameExporter(
    const std::string& path, uint32_t width, uint32_t height, uint32_t total_frames)
: _path{path}
, _width{width}
, _height{height}
, _total_frames{total_frames}
, _frame{0}
{
}

void FrameExporter::encode_frame(const uint8_t* data)
{
  auto counter_str = std::to_string(_frame);
  std::size_t padding =
      std::to_string(_total_frames).length() - counter_str.length();
  std::size_t index = _path.find_last_of('.');
  auto frame_path = _path.substr(0, index) + '_' +
      std::string(padding, '0') + counter_str + _path.substr(index);

  sf::Image image;
  image.create(_width, _height, data);
  image.saveToFile(frame_path);
  ++_frame;
}

WebmExporter::WebmExporter(
    const std::string& path, uint32_t width, uint32_t height,
    uint32_t fps, uint32_t bitrate)
: _success{false}
, _width{width}
, _height{height}
, _fps{fps}
, _video_track{0}
, _img{nullptr}
, _frame_index{0}
{
  if (!_writer.Open(path.c_str())) {
    std::cerr << "couldn't open " << path << " for writing" << std::endl;
    return;
  }

  if (!_segment.Init(&_writer)) {
    std::cerr << "couldn't initialise muxer segment" << std::endl;
    return;
  }
  _segment.set_mode(mkvmuxer::Segment::kFile);
  _segment.OutputCues(true);
  _segment.GetSegmentInfo()->set_writing_app("trance");

  _video_track = _segment.AddVideoTrack(width, height, 0);
  if (!_video_track) {
    std::cerr << "couldn't add video track" << std::endl;
    return;
  }

  auto video = (mkvmuxer::VideoTrack*)
      _segment.GetTrackByNumber(_video_track);
  if (!video) {
    std::cerr << "couldn't get video track" << std::endl;
    return;
  }
  video->set_frame_rate(fps);
  _segment.CuesTrack(_video_track);

  // See http://www.webmproject.org/docs/encoder-parameters.
  // TODO: tweak this a bunch. Especially: 2-pass, buffer size, quality
  // options, and multithreaded encoding.
  vpx_codec_enc_cfg_t cfg;
  if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &cfg, 0)) {
    std::cerr << "couldn't get default codec config" << std::endl;
    return;
  }
  cfg.g_w = width;
  cfg.g_h = height;
  cfg.g_timebase.num = 1;
  cfg.g_timebase.den = fps;
  cfg.rc_target_bitrate = bitrate;

  if (vpx_codec_enc_init(&_codec, vpx_codec_vp8_cx(), &cfg, 0)) {
    codec_error("couldn't initialise encoder");
    return;
  }

  _img = vpx_img_alloc(nullptr, VPX_IMG_FMT_I420, width, height, 16);
  if (!_img) {
    std::cerr << "couldn't allocate image for encoding" << std::endl;
    return;
  }
  _success = true;
}

bool WebmExporter::success() const
{
  return _success;
}

void WebmExporter::encode_frame(const uint8_t* data)
{
  // Convert YUV to YUV420.
  for (uint32_t y = 0; y < _height; ++y) {
    for (uint32_t x = 0; x < _width; ++x) {
      _img->planes[VPX_PLANE_Y]
          [x + y * _img->stride[VPX_PLANE_Y]] = data[4 * (x + y * _width)];
    }
  }
  for (uint32_t y = 0; y < _height / 2; ++y) {
    for (uint32_t x = 0; x < _width / 2; ++x) {
      auto c00 = 4 * (2 * x + 2 * y * _width);
      auto c01 = 4 * (2 * x + (1 + 2 * y) * _width);
      auto c10 = 4 * (1 + 2 * x + 2 * y * _width);
      auto c11 = 4 * (1 + 2 * x + (1 + 2 * y) * _width);

      _img->planes[VPX_PLANE_U][x + y * _img->stride[VPX_PLANE_U]] =
          (data[1 + c00] + data[1 + c01] + data[1 + c10] + data[1 + c11]) / 4;
      _img->planes[VPX_PLANE_V][x + y * _img->stride[VPX_PLANE_V]] =
          (data[2 + c00] + data[2 + c01] + data[2 + c10] + data[2 + c11]) / 4;
    }
  }
  add_frame(_img);
}

void WebmExporter::codec_error(const std::string& s)
{
  auto detail = vpx_codec_error_detail(&_codec);
  std::cerr << s << ": " << vpx_codec_error(&_codec);
  if (detail) {
    std::cerr << ": " << detail;
  }
  std::cerr << std::endl;
}

bool WebmExporter::add_frame(const vpx_image* data)
{
  // TODO: option for VPX_DL_BEST_QUALITY?
  auto result = vpx_codec_encode(
      &_codec, data, _frame_index++, 1, 0, VPX_DL_GOOD_QUALITY);
  if (result != VPX_CODEC_OK) {
    codec_error("couldn't encode frame");
    return false;
  }

  vpx_codec_iter_t iter = nullptr;
  const vpx_codec_cx_pkt_t* packet = nullptr;
  bool found_packet = false;
  while (packet = vpx_codec_get_cx_data(&_codec, &iter)) {
    found_packet = true;
    if (packet->kind != VPX_CODEC_CX_FRAME_PKT) {
      continue;
    }
    auto timestamp_ns = 1000000000 * packet->data.frame.pts / _fps;
    bool result = _segment.AddFrame(
        (uint8_t*) packet->data.frame.buf, packet->data.frame.sz, _video_track,
        timestamp_ns, packet->data.frame.flags & VPX_FRAME_IS_KEY);
    if (!result) {
      std::cerr << "couldn't add frame" << std::endl;
      return false;
    }
  }

  return found_packet;
};

WebmExporter::~WebmExporter()
{
  if (_img) {
    vpx_img_free(_img);
  }
  // Flush encoder.
  while (add_frame(nullptr));

  if (vpx_codec_destroy(&_codec)) {
    codec_error("failed to destroy codec");
    return;
  }
  if (!_segment.Finalize()) {
    std::cerr << "couldn't finalise muxer segment" << std::endl;
    return;
  }
  _writer.Close();
}
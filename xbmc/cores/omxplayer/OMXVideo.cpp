/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#elif defined(_WIN32)
#include "system.h"
#endif

#include "OMXVideo.h"

#include "utils/log.h"
#include "linux/XMemUtils.h"
#include "DVDDemuxers/DVDDemuxUtils.h"
#include "settings/AdvancedSettings.h"
#include "xbmc/guilib/GraphicContext.h"
#include "settings/Settings.h"
#include "utils/BitstreamConverter.h"
#include "boblight.h"


#include <sys/time.h>
#include <inttypes.h>

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXVideo"

#if 0
// TODO: These are Nvidia Tegra2 dependent, need to dynamiclly find the
// right codec matched to video format.
#define OMX_H264BASE_DECODER    "OMX.Nvidia.h264.decode"
// OMX.Nvidia.h264ext.decode segfaults, not sure why.
//#define OMX_H264MAIN_DECODER  "OMX.Nvidia.h264ext.decode"
#define OMX_H264MAIN_DECODER    "OMX.Nvidia.h264.decode"
#define OMX_H264HIGH_DECODER    "OMX.Nvidia.h264ext.decode"
#define OMX_MPEG4_DECODER       "OMX.Nvidia.mp4.decode"
#define OMX_MPEG4EXT_DECODER    "OMX.Nvidia.mp4ext.decode"
#define OMX_MPEG2V_DECODER      "OMX.Nvidia.mpeg2v.decode"
#define OMX_VC1_DECODER         "OMX.Nvidia.vc1.decode"
#endif

#define OMX_VIDEO_DECODER       "OMX.broadcom.video_decode"
#define OMX_H264BASE_DECODER    OMX_VIDEO_DECODER
#define OMX_H264MAIN_DECODER    OMX_VIDEO_DECODER
#define OMX_H264HIGH_DECODER    OMX_VIDEO_DECODER
#define OMX_MPEG4_DECODER       OMX_VIDEO_DECODER
#define OMX_MSMPEG4V1_DECODER   OMX_VIDEO_DECODER
#define OMX_MSMPEG4V2_DECODER   OMX_VIDEO_DECODER
#define OMX_MSMPEG4V3_DECODER   OMX_VIDEO_DECODER
#define OMX_MPEG4EXT_DECODER    OMX_VIDEO_DECODER
#define OMX_MPEG2V_DECODER      OMX_VIDEO_DECODER
#define OMX_VC1_DECODER         OMX_VIDEO_DECODER
#define OMX_WMV3_DECODER        OMX_VIDEO_DECODER
#define OMX_VP6_DECODER         OMX_VIDEO_DECODER
#define OMX_VP8_DECODER         OMX_VIDEO_DECODER
#define OMX_THEORA_DECODER      OMX_VIDEO_DECODER
#define OMX_MJPEG_DECODER       OMX_VIDEO_DECODER

#define MAX_TEXT_LENGTH 1024

void*            COMXVideo::m_boblight;
unsigned int     COMXVideo::m_boblight_margin_t;
unsigned int     COMXVideo::m_boblight_margin_b;
unsigned int     COMXVideo::m_boblight_margin_l;
unsigned int     COMXVideo::m_boblight_margin_r;
int              COMXVideo::m_boblight_width;
int              COMXVideo::m_boblight_height;
int              COMXVideo::m_boblight_timeout;
bool volatile    COMXVideo::m_boblight_threadstop;
COMXCoreComponent COMXVideo::m_omx_split;
COMXCoreComponent COMXVideo::m_omx_resize;

OMX_BUFFERHEADERTYPE* COMXVideo::m_boblight_bufferpointer;

pthread_t        COMXVideo::m_boblight_clientthread;

pthread_mutex_t  COMXVideo::m_boblight_bufferdone_mutex;
pthread_cond_t   COMXVideo::m_boblight_bufferdone_cond;
bool volatile    COMXVideo::m_boblight_bufferdone_flag;

COMXVideo::COMXVideo() : m_video_codec_name("")
{
  m_is_open           = false;
  m_Pause             = false;
  m_extradata         = NULL;
  m_extrasize         = 0;
  m_video_convert     = false;
  m_deinterlace       = false;
  m_hdmi_clock_sync   = false;
  m_drop_state        = false;
  m_decoded_width     = 0;
  m_decoded_height    = 0;
  m_omx_clock         = NULL;
  m_av_clock          = NULL;
  m_res_callback      = NULL;
  m_res_ctx           = NULL;
  m_submitted_eos     = false;
  COMXVideo::m_boblight = NULL;
  COMXVideo::m_boblight_margin_t=0;
  COMXVideo::m_boblight_margin_b=0;
  COMXVideo::m_boblight_margin_l=0;
  COMXVideo::m_boblight_margin_r=0;
  COMXVideo::m_boblight_width=0;
  COMXVideo::m_boblight_height=0;
  COMXVideo::m_boblight_timeout=0;
  COMXVideo::m_boblight_threadstop=false;
  COMXVideo::m_boblight_bufferdone_flag=false;
}

COMXVideo::~COMXVideo()
{
  Close();
}

bool COMXVideo::SendDecoderConfig()
{
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;

  /* send decoder config */
  if(m_extrasize > 0 && m_extradata != NULL)
  {
    OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();

    if(omx_buffer == NULL)
    {
      CLog::Log(LOGERROR, "%s::%s - buffer error 0x%08x", CLASSNAME, __func__, omx_err);
      return false;
    }

    omx_buffer->nOffset = 0;
    omx_buffer->nFilledLen = m_extrasize;
    if(omx_buffer->nFilledLen > omx_buffer->nAllocLen)
    {
      CLog::Log(LOGERROR, "%s::%s - omx_buffer->nFilledLen > omx_buffer->nAllocLen", CLASSNAME, __func__);
      return false;
    }

    memset((unsigned char *)omx_buffer->pBuffer, 0x0, omx_buffer->nAllocLen);
    memcpy((unsigned char *)omx_buffer->pBuffer, m_extradata, omx_buffer->nFilledLen);
    omx_buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;
  
    omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
  }
  return true;
}

bool COMXVideo::NaluFormatStartCodes(enum CodecID codec, uint8_t *in_extradata, int in_extrasize)
{
  switch(codec)
  {
    case CODEC_ID_H264:
      if (in_extrasize < 7 || in_extradata == NULL)
        return true;
      // valid avcC atom data always starts with the value 1 (version), otherwise annexb
      else if ( *in_extradata != 1 )
        return true;
    default: break;
  }
  return false;    
}

bool COMXVideo::Open(CDVDStreamInfo &hints, OMXClock *clock, bool deinterlace, bool hdmi_clock_sync, void* boblight_instance, int boblight_sizedown, int boblight_margin, int boblight_timeout)
{
  bool vflip = false;
  Close();

  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;
  std::string decoder_name;

  m_res_ctx           = NULL;
  m_res_callback      = NULL;

  m_video_codec_name      = "";
  m_codingType            = OMX_VIDEO_CodingUnused;

  m_decoded_width  = hints.width;
  m_decoded_height = hints.height;

  m_hdmi_clock_sync = hdmi_clock_sync;
  m_submitted_eos = false;

 //copy boblight parameter
  m_boblight_sizedown = boblight_sizedown;
  m_boblight_margin = boblight_margin;
  COMXVideo::m_boblight = boblight_instance;
  COMXVideo::m_boblight_timeout = boblight_timeout;

  if(!m_decoded_width || !m_decoded_height)
    return false;

  if(hints.extrasize > 0 && hints.extradata != NULL)
  {
    m_extrasize = hints.extrasize;
    m_extradata = (uint8_t *)malloc(m_extrasize);
    memcpy(m_extradata, hints.extradata, hints.extrasize);
  }

  switch (hints.codec)
  {
    case CODEC_ID_H264:
    {
      switch(hints.profile)
      {
        case FF_PROFILE_H264_BASELINE:
          // (role name) video_decoder.avc
          // H.264 Baseline profile
          decoder_name = OMX_H264BASE_DECODER;
          m_codingType = OMX_VIDEO_CodingAVC;
          m_video_codec_name = "omx-h264";
          break;
        case FF_PROFILE_H264_MAIN:
          // (role name) video_decoder.avc
          // H.264 Main profile
          decoder_name = OMX_H264MAIN_DECODER;
          m_codingType = OMX_VIDEO_CodingAVC;
          m_video_codec_name = "omx-h264";
          break;
        case FF_PROFILE_H264_HIGH:
          // (role name) video_decoder.avc
          // H.264 Main profile
          decoder_name = OMX_H264HIGH_DECODER;
          m_codingType = OMX_VIDEO_CodingAVC;
          m_video_codec_name = "omx-h264";
          break;
        case FF_PROFILE_UNKNOWN:
          decoder_name = OMX_H264HIGH_DECODER;
          m_codingType = OMX_VIDEO_CodingAVC;
          m_video_codec_name = "omx-h264";
          break;
        default:
          decoder_name = OMX_H264HIGH_DECODER;
          m_codingType = OMX_VIDEO_CodingAVC;
          m_video_codec_name = "omx-h264";
          break;
      }

      /* check interlaced */
      if(m_extrasize > 9 && m_extradata[0] == 1)
      {
        CBitstreamConverter converter;
        converter.Open(hints.codec, (uint8_t *)hints.extradata, hints.extrasize, true);

        int32_t  max_ref_frames = 0;
        uint8_t  *spc = m_extradata + 6;
        uint32_t sps_size = BS_RB16(spc);
        bool     interlaced = true;
        if (sps_size)
          converter.parseh264_sps(spc+3, sps_size-1, &interlaced, &max_ref_frames);
        if(!interlaced && deinterlace)
          deinterlace = false;

        converter.Close();
      }
    }
    break;
    case CODEC_ID_MPEG4:
      // (role name) video_decoder.mpeg4
      // MPEG-4, DivX 4/5 and Xvid compatible
      decoder_name = OMX_MPEG4_DECODER;
      m_codingType = OMX_VIDEO_CodingMPEG4;
      m_video_codec_name = "omx-mpeg4";
      break;
    case CODEC_ID_MPEG1VIDEO:
    case CODEC_ID_MPEG2VIDEO:
      // (role name) video_decoder.mpeg2
      // MPEG-2
      decoder_name = OMX_MPEG2V_DECODER;
      m_codingType = OMX_VIDEO_CodingMPEG2;
      m_video_codec_name = "omx-mpeg2";
      break;
    case CODEC_ID_H263:
      // (role name) video_decoder.mpeg4
      // MPEG-4, DivX 4/5 and Xvid compatible
      decoder_name = OMX_MPEG4_DECODER;
      m_codingType = OMX_VIDEO_CodingMPEG4;
      m_video_codec_name = "omx-h263";
      break;
    case CODEC_ID_VP6:
      // this form is encoded upside down
      vflip = true;
      // fall through
    case CODEC_ID_VP6F:
    case CODEC_ID_VP6A:
      // (role name) video_decoder.vp6
      // VP6
      decoder_name = OMX_VP6_DECODER;
      m_codingType = OMX_VIDEO_CodingVP6;
      m_video_codec_name = "omx-vp6";
    break;
    case CODEC_ID_VP8:
      // (role name) video_decoder.vp8
      // VP8
      decoder_name = OMX_VP8_DECODER;
      m_codingType = OMX_VIDEO_CodingVP8;
      m_video_codec_name = "omx-vp8";
    break;
    case CODEC_ID_THEORA:
      // (role name) video_decoder.theora
      // theora
      decoder_name = OMX_THEORA_DECODER;
      m_codingType = OMX_VIDEO_CodingTheora;
      m_video_codec_name = "omx-theora";
    break;
    case CODEC_ID_MJPEG:
    case CODEC_ID_MJPEGB:
      // (role name) video_decoder.mjpg
      // mjpg
      decoder_name = OMX_MJPEG_DECODER;
      m_codingType = OMX_VIDEO_CodingMJPEG;
      m_video_codec_name = "omx-mjpeg";
    break;
    case CODEC_ID_VC1:
    case CODEC_ID_WMV3:
      // (role name) video_decoder.vc1
      // VC-1, WMV9
      decoder_name = OMX_VC1_DECODER;
      m_codingType = OMX_VIDEO_CodingWMV;
      m_video_codec_name = "omx-vc1";
      break;
    default:
      return false;
    break;
  }

  /* enable deintelace on SD and 1080i */
  if(m_decoded_width <= 720 && m_decoded_height <=576 && deinterlace)
    m_deinterlace = deinterlace;
  else if(m_decoded_width >= 1920 && m_decoded_height >= 540 && deinterlace)
    m_deinterlace = deinterlace;

  if(m_deinterlace)
    CLog::Log(LOGDEBUG, "COMXVideo::Open : enable deinterlace\n");

  std::string componentName = decoder_name;
  if(!m_omx_decoder.Initialize((const std::string)componentName, OMX_IndexParamVideoInit))
    return false;

  componentName = "OMX.broadcom.video_render";
  if(!m_omx_render.Initialize((const std::string)componentName, OMX_IndexParamVideoInit))
    return false;

  m_omx_render.ResetEos();

  componentName = "OMX.broadcom.video_scheduler";
  if(!m_omx_sched.Initialize((const std::string)componentName, OMX_IndexParamVideoInit))
    return false;

  if(COMXVideo::m_boblight){
    componentName = "OMX.broadcom.video_splitter";
    if(!m_omx_split.Initialize((const std::string)componentName, OMX_IndexParamVideoInit))
      return false;

    componentName = "OMX.broadcom.resize";
    if(!m_omx_resize.Initialize((const std::string)componentName, OMX_IndexParamImageInit))
      return false;
  }

  if(m_deinterlace)
  {
    componentName = "OMX.broadcom.image_fx";
    if(!m_omx_image_fx.Initialize((const std::string)componentName, OMX_IndexParamImageInit))
      return false;
  }

  OMX_VIDEO_PARAM_PORTFORMATTYPE formatType;
  /*
  OMX_INIT_STRUCTURE(formatType);
  formatType.nPortIndex = m_omx_decoder.GetInputPort();
  OMX_U32 nIndex = 1;
  bool bFound = false;

  omx_err = OMX_ErrorNone;
  do
  {
    formatType.nIndex = nIndex;
    omx_err = m_omx_decoder.GetParameter(OMX_IndexParamVideoPortFormat, &formatType);
    if(formatType.eCompressionFormat == m_codingType)
    {
      bFound = true;
      break;
    }
    nIndex++;
  }
  while(omx_err == OMX_ErrorNone);

  if(!bFound)
  {
    CLog::Log(LOGINFO, "COMXVideo::Open coding : %s not supported\n", m_video_codec_name.c_str());
    return false;
  }
  */

  if(clock == NULL)
    return false;

  m_av_clock = clock;
  m_omx_clock = m_av_clock->GetOMXClock();

  if(m_omx_clock->GetComponent() == NULL)
  {
    m_av_clock = NULL;
    m_omx_clock = NULL;
    return false;
  }

  if(m_deinterlace)
  {
    m_omx_tunnel_decoder.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_image_fx, m_omx_image_fx.GetInputPort());
    m_omx_tunnel_image_fx.Initialize(&m_omx_image_fx, m_omx_image_fx.GetOutputPort(), &m_omx_sched, m_omx_sched.GetInputPort());
  }
  else
  {
    m_omx_tunnel_decoder.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_sched, m_omx_sched.GetInputPort());
  }

 if(COMXVideo::m_boblight){
    m_omx_tunnel_sched.Initialize(&m_omx_sched, m_omx_sched.GetOutputPort(), &m_omx_split, m_omx_split.GetInputPort());
    m_omx_tunnel_split.Initialize(&m_omx_split, m_omx_split.GetOutputPort()+1, &m_omx_render, m_omx_render.GetInputPort());
    m_omx_tunnel_resize.Initialize(&m_omx_split, m_omx_split.GetOutputPort(), &m_omx_resize, m_omx_resize.GetInputPort());
  }
  else
  {
    m_omx_tunnel_sched.Initialize(&m_omx_sched, m_omx_sched.GetOutputPort(), &m_omx_render, m_omx_render.GetInputPort());
  }
  m_omx_tunnel_clock.Initialize(m_omx_clock, m_omx_clock->GetInputPort()  + 1, &m_omx_sched, m_omx_sched.GetOutputPort()  + 1);

  omx_err = m_omx_tunnel_clock.Establish(false);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open m_omx_tunnel_clock.Establish\n");
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateIdle);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open m_omx_decoder.SetStateForComponent\n");
    return false;
  }

  OMX_INIT_STRUCTURE(formatType);
  formatType.nPortIndex = m_omx_decoder.GetInputPort();
  formatType.eCompressionFormat = m_codingType;

  if (hints.fpsscale > 0 && hints.fpsrate > 0)
  {
    formatType.xFramerate = (long long)(1<<16)*hints.fpsrate / hints.fpsscale;
  }
  else
  {
    formatType.xFramerate = 25 * (1<<16);
  }

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamVideoPortFormat, &formatType);
  if(omx_err != OMX_ErrorNone)
    return false;
  
  OMX_PARAM_PORTDEFINITIONTYPE portParam;
  OMX_INIT_STRUCTURE(portParam);
  portParam.nPortIndex = m_omx_decoder.GetInputPort();

  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &portParam);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open error OMX_IndexParamPortDefinition omx_err(0x%08x)\n", omx_err);
    return false;
  }

  portParam.nPortIndex = m_omx_decoder.GetInputPort();
  portParam.nBufferCountActual = VIDEO_BUFFERS;

  portParam.format.video.nFrameWidth  = m_decoded_width;
  portParam.format.video.nFrameHeight = m_decoded_height;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &portParam);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open error OMX_IndexParamPortDefinition omx_err(0x%08x)\n", omx_err);
    return false;
  }

  OMX_PARAM_BRCMVIDEODECODEERRORCONCEALMENTTYPE concanParam;
  OMX_INIT_STRUCTURE(concanParam);
  if(g_advancedSettings.m_omxDecodeStartWithValidFrame)
    concanParam.bStartWithValidFrame = OMX_TRUE;
  else
    concanParam.bStartWithValidFrame = OMX_FALSE;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamBrcmVideoDecodeErrorConcealment, &concanParam);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open error OMX_IndexParamBrcmVideoDecodeErrorConcealment omx_err(0x%08x)\n", omx_err);
    return false;
  }

  if (m_deinterlace)
  {
    // the deinterlace component requires 3 additional video buffers in addition to the DPB (this is normally 2).
    OMX_PARAM_U32TYPE extra_buffers;
    OMX_INIT_STRUCTURE(extra_buffers);
    extra_buffers.nU32 = 3;

    omx_err = m_omx_decoder.SetParameter(OMX_IndexParamBrcmExtraBuffers, &extra_buffers);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open error OMX_IndexParamBrcmExtraBuffers omx_err(0x%08x)\n", omx_err);
      return false;
    }
  }

  // broadcom omx entension:
  // When enabled, the timestamp fifo mode will change the way incoming timestamps are associated with output images.
  // In this mode the incoming timestamps get used without re-ordering on output images.
  if(hints.ptsinvalid)
  {
    OMX_CONFIG_BOOLEANTYPE timeStampMode;
    OMX_INIT_STRUCTURE(timeStampMode);
    timeStampMode.bEnabled = OMX_TRUE;

    omx_err = m_omx_decoder.SetParameter((OMX_INDEXTYPE)OMX_IndexParamBrcmVideoTimestampFifo, &timeStampMode);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open OMX_IndexParamBrcmVideoTimestampFifo error (0%08x)\n", omx_err);
      return false;
    }
  }

  if(NaluFormatStartCodes(hints.codec, m_extradata, m_extrasize))
  {
    OMX_NALSTREAMFORMATTYPE nalStreamFormat;
    OMX_INIT_STRUCTURE(nalStreamFormat);
    nalStreamFormat.nPortIndex = m_omx_decoder.GetInputPort();
    nalStreamFormat.eNaluFormat = OMX_NaluFormatStartCodes;

    omx_err = m_omx_decoder.SetParameter((OMX_INDEXTYPE)OMX_IndexParamNalStreamFormatSelect, &nalStreamFormat);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open OMX_IndexParamNalStreamFormatSelect error (0%08x)\n", omx_err);
      return false;
    }
  }

  if(m_hdmi_clock_sync)
  {
    OMX_CONFIG_LATENCYTARGETTYPE latencyTarget;
    OMX_INIT_STRUCTURE(latencyTarget);
    latencyTarget.nPortIndex = m_omx_render.GetInputPort();
    latencyTarget.bEnabled = OMX_TRUE;
    latencyTarget.nFilter = 2;
    latencyTarget.nTarget = 4000;
    latencyTarget.nShift = 3;
    latencyTarget.nSpeedFactor = -135;
    latencyTarget.nInterFactor = 500;
    latencyTarget.nAdjCap = 20;

    omx_err = m_omx_render.SetConfig(OMX_IndexConfigLatencyTarget, &latencyTarget);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open OMX_IndexConfigLatencyTarget error (0%08x)\n", omx_err);
      return false;
    }
  }

  // Alloc buffers for the omx intput port.
  omx_err = m_omx_decoder.AllocInputBuffers();
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open AllocOMXInputBuffers error (0%08x)\n", omx_err);
    return false;
  }

  omx_err = m_omx_tunnel_decoder.Establish(false);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open m_omx_tunnel_decoder.Establish\n");
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open error m_omx_decoder.SetStateForComponent\n");
    return false;
  }

  if(m_deinterlace)
  {
    OMX_CONFIG_IMAGEFILTERPARAMSTYPE image_filter;
    OMX_INIT_STRUCTURE(image_filter);

    image_filter.nPortIndex = m_omx_image_fx.GetOutputPort();
    image_filter.nNumParams = 1;
    image_filter.nParams[0] = 3;
    image_filter.eImageFilter = OMX_ImageFilterDeInterlaceAdvanced;

    omx_err = m_omx_image_fx.SetConfig(OMX_IndexConfigCommonImageFilterParameters, &image_filter);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open error OMX_IndexConfigCommonImageFilterParameters omx_err(0x%08x)\n", omx_err);
      return false;
    }

    omx_err = m_omx_tunnel_image_fx.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open m_omx_tunnel_image_fx.Establish\n");
      return false;
    }

    omx_err = m_omx_image_fx.SetStateForComponent(OMX_StateExecuting);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open error m_omx_image_fx.SetStateForComponent\n");
      return false;
    }

    m_omx_image_fx.DisablePort(m_omx_image_fx.GetInputPort(), false);
    m_omx_image_fx.DisablePort(m_omx_image_fx.GetOutputPort(), false);
  }

  omx_err = m_omx_tunnel_sched.Establish(false);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open m_omx_tunnel_sched.Establish\n");
    return false;
  }

  omx_err = m_omx_sched.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open error m_omx_sched.SetStateForComponent\n");
    return false;
  }

 if(COMXVideo::m_boblight){
    omx_err = m_omx_split.SetStateForComponent(OMX_StateExecuting);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open error m_omx_split.SetStateForComponent\n");
      return false;
    }

    //set up the resizer
    //make sure output of the splitter and input of the resize match
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    OMX_INIT_STRUCTURE(port_def);

    port_def.nPortIndex = m_omx_split.GetOutputPort();
    m_omx_split.GetParameter(OMX_IndexParamPortDefinition, &port_def);
    port_def.nPortIndex = m_omx_resize.GetInputPort();
    m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);

    omx_err = m_omx_tunnel_resize.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_tunnel_resize.Establish\n", CLASSNAME, __func__);
      return false;
    }
    omx_err = m_omx_resize.WaitForEvent(OMX_EventPortSettingsChanged);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.WaitForEvent=%x\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    port_def.nPortIndex = m_omx_resize.GetOutputPort();
    m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &port_def);

    port_def.nPortIndex = m_omx_resize.GetOutputPort();
    port_def.format.image.eCompressionFormat = OMX_IMAGE_CodingUnused;
    port_def.format.image.eColorFormat = OMX_COLOR_Format32bitARGB8888;
    //calculate the size of the sized-down image
    if(m_boblight_sizedown%2==1)m_boblight_sizedown--; //make sure we have even dimensions, since resize component requires it
    float factor;
    if(m_decoded_width>m_decoded_height){
      factor = (float)m_boblight_sizedown / m_decoded_width;
    }else{
      factor = (float)m_boblight_sizedown / m_decoded_height;
    }
    port_def.format.image.nFrameWidth = round(factor * m_decoded_width);
    port_def.format.image.nFrameHeight = round(factor * m_decoded_height);
    port_def.format.image.nStride = 0;
    port_def.format.image.nSliceHeight = 0;
    port_def.format.image.bFlagErrorConcealment = OMX_FALSE;

    omx_err = m_omx_resize.SetParameter(OMX_IndexParamPortDefinition, &port_def);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.SetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    COMXVideo::m_boblight_width = (int)round(factor * m_decoded_width);
    COMXVideo::m_boblight_height = (int)round(factor * m_decoded_height);
    //calculate margins of processed pixels on the outer border of the image
    COMXVideo::m_boblight_margin_t = (int)round(m_boblight_margin*m_boblight_height/100);
    COMXVideo::m_boblight_margin_b = m_boblight_height - m_boblight_margin_t;
    COMXVideo::m_boblight_margin_l = (int)round(m_boblight_margin*m_boblight_width/100);
    COMXVideo::m_boblight_margin_r = m_boblight_width - m_boblight_margin_l;
    CLog::Log(LOGDEBUG, "Setting boblight scanrange to %ix%i, scan margin is %i percent\n", COMXVideo::m_boblight_width, COMXVideo::m_boblight_height, m_boblight_margin);
    boblight_setscanrange(COMXVideo::m_boblight, COMXVideo::m_boblight_width, COMXVideo::m_boblight_height);

    OMX_PARAM_PORTDEFINITIONTYPE  m_decoded_format;
    OMX_INIT_STRUCTURE(m_decoded_format);
    m_decoded_format.nPortIndex = m_omx_resize.GetOutputPort();
    omx_err = m_omx_resize.GetParameter(OMX_IndexParamPortDefinition, &m_decoded_format);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.GetParameter result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }
    assert(m_decoded_format.nBufferCountActual == 1);

    omx_err = m_omx_resize.AllocOutputBuffers();
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.AllocOutputBuffers result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    omx_err = m_omx_resize.SetStateForComponent(OMX_StateExecuting);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s m_omx_resize.SetStateForComponent result(0x%x)\n", CLASSNAME, __func__, omx_err);
      return false;
    }

    omx_err = m_omx_tunnel_split.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open m_omx_tunnel_split.Establish\n");
      return false;
    }

    omx_err = m_omx_tunnel_resize.Establish(false);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open m_omx_tunnel_resize.Establish\n");
      return false;
    }

    //setting the custom callback to broadcast a signal COMXVideo::Thread is waiting for
    m_omx_resize.SetCustomDecoderFillBufferDoneHandler(&COMXVideo::BufferDoneHandler);

    //prepare boblight client thread and start it
    pthread_cond_init(&COMXVideo::m_boblight_bufferdone_cond, NULL);
    pthread_mutex_init(&COMXVideo::m_boblight_bufferdone_mutex, NULL);

    COMXCoreComponent* args[2];
    args[0] = &m_omx_split;
    args[1] = &m_omx_resize;

    pthread_create(&COMXVideo::m_boblight_clientthread, NULL, &COMXVideo::BoblightClientThread, (void*)&args);
  }

  omx_err = m_omx_render.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open error m_omx_render.SetStateForComponent\n");
    return false;
  }

  if(!SendDecoderConfig())
    return false;

  m_is_open           = true;
  m_drop_state        = false;

  OMX_CONFIG_DISPLAYREGIONTYPE configDisplay;
  OMX_INIT_STRUCTURE(configDisplay);
  configDisplay.nPortIndex = m_omx_render.GetInputPort();

  configDisplay.set = OMX_DISPLAY_SET_TRANSFORM;

  switch(hints.orientation)
  {
    case 90:
      configDisplay.transform = OMX_DISPLAY_ROT90;
      break;
    case 180:
      configDisplay.transform = OMX_DISPLAY_ROT180;
      break;
    case 270:
      configDisplay.transform = OMX_DISPLAY_ROT270;
      break;
    default:
      configDisplay.transform = OMX_DISPLAY_ROT0;
      break;
  }
  if (vflip)
      configDisplay.transform = OMX_DISPLAY_MIRROR_ROT180;

  omx_err = m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGWARNING, "COMXVideo::Open could not set orientation : %d\n", hints.orientation);
  }

  /*
  configDisplay.set     = OMX_DISPLAY_SET_LAYER;
  configDisplay.layer   = 2;

  omx_err = m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);
  if(omx_err != OMX_ErrorNone)
    return false;

  configDisplay.set     = OMX_DISPLAY_SET_DEST_RECT;
  configDisplay.dest_rect.x_offset  = 100;
  configDisplay.dest_rect.y_offset  = 100;
  configDisplay.dest_rect.width     = 640;
  configDisplay.dest_rect.height    = 480;
    
  omx_err = m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);
  if(omx_err != OMX_ErrorNone)
    return false;

  configDisplay.set     = OMX_DISPLAY_SET_TRANSFORM;
  configDisplay.transform = OMX_DISPLAY_ROT180;
    
  omx_err = m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);
  if(omx_err != OMX_ErrorNone)
    return false;

  configDisplay.set     = OMX_DISPLAY_SET_FULLSCREEN;
  configDisplay.fullscreen = OMX_FALSE;
    
  omx_err = m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);
  if(omx_err != OMX_ErrorNone)
    return false;

  configDisplay.set     = OMX_DISPLAY_SET_MODE;
  configDisplay.mode    = OMX_DISPLAY_MODE_FILL; //OMX_DISPLAY_MODE_LETTERBOX;
    
  omx_err = m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);
  if(omx_err != OMX_ErrorNone)
    return false;

  configDisplay.set     = OMX_DISPLAY_SET_LAYER;
  configDisplay.layer   = 1;

  omx_err = m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);
  if(omx_err != OMX_ErrorNone)
    return false;

  configDisplay.set     = OMX_DISPLAY_SET_ALPHA;
  configDisplay.alpha   = OMX_FALSE;
    
  omx_err = m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);
  if(omx_err != OMX_ErrorNone)
    return false;

  */

  if(m_omx_decoder.BadState())
    return false;

  CLog::Log(LOGDEBUG,
    "%s::%s - decoder_component(0x%p), input_port(0x%x), output_port(0x%x) deinterlace %d hdmiclocksync %d\n",
    CLASSNAME, __func__, m_omx_decoder.GetComponent(), m_omx_decoder.GetInputPort(), m_omx_decoder.GetOutputPort(),
    m_deinterlace, m_hdmi_clock_sync);

  // start from assuming all recent frames had valid pts
  m_history_valid_pts = ~0;

  return true;
}

void COMXVideo::Close()
{
  if(COMXVideo::m_boblight){
    //signal thread to stop
    COMXVideo::m_boblight_threadstop = true;
    //wait for thread to terminate nicely
    //pthread_join(COMXVideo::m_boblight_clientthread, NULL);
    //cleanup thread related stuff
    pthread_mutex_destroy(&COMXVideo::m_boblight_bufferdone_mutex);
    pthread_cond_destroy(&COMXVideo::m_boblight_bufferdone_cond); 
  }

  //close components and tunnels
  m_omx_tunnel_decoder.Flush();
  if(m_deinterlace)
    m_omx_tunnel_image_fx.Flush();
  m_omx_tunnel_clock.Flush();
  m_omx_tunnel_sched.Flush();
  if(COMXVideo::m_boblight){
    m_omx_tunnel_split.Flush();
    m_omx_tunnel_resize.Flush();
  }


  m_omx_tunnel_clock.Deestablish();
  m_omx_tunnel_decoder.Deestablish();
  if(m_deinterlace)
    m_omx_tunnel_image_fx.Deestablish();
  m_omx_tunnel_sched.Deestablish();
  if(COMXVideo::m_boblight){
    m_omx_tunnel_split.Deestablish();
    m_omx_tunnel_resize.Deestablish();
  }
 

  m_omx_decoder.FlushInput();

  m_omx_sched.Deinitialize(true);
  m_omx_decoder.Deinitialize(true);
  if(COMXVideo::m_boblight){
    m_omx_split.Deinitialize();
    m_omx_resize.Deinitialize();
  }
  if(m_deinterlace)
    m_omx_image_fx.Deinitialize(true);
  m_omx_render.Deinitialize(true);

  m_is_open       = false;

  if(m_extradata)
    free(m_extradata);
  m_extradata = NULL;
  m_extrasize = 0;

  m_video_convert     = false;
  m_video_codec_name  = "";
  m_deinterlace       = false;
  m_av_clock          = NULL;

  m_res_ctx           = NULL;
  m_res_callback      = NULL;

  COMXVideo::m_boblight_threadstop=false;
  COMXVideo::m_boblight_bufferdone_flag=false;
  COMXVideo::m_boblight=NULL;
}

void COMXVideo::SetDropState(bool bDrop)
{
  m_drop_state = bDrop;
}

unsigned int COMXVideo::GetFreeSpace()
{
  return m_omx_decoder.GetInputBufferSpace();
}

unsigned int COMXVideo::GetSize()
{
  return m_omx_decoder.GetInputBufferSize();
}

OMX_ERRORTYPE COMXVideo::BufferDoneHandler(OMX_HANDLETYPE hComponent, OMX_PTR pAppData, OMX_BUFFERHEADERTYPE* pBuffer){
  pthread_mutex_lock(&COMXVideo::m_boblight_bufferdone_mutex);
  COMXVideo::m_boblight_bufferpointer = pBuffer;
  COMXVideo::m_boblight_bufferdone_flag = true;
  pthread_cond_broadcast(&COMXVideo::m_boblight_bufferdone_cond);
  pthread_mutex_unlock(&COMXVideo::m_boblight_bufferdone_mutex);
  return OMX_ErrorNone;
}

void* COMXVideo::BoblightClientThread(void* data){
  int rgb[3];
  unsigned int offset = 0;
  uint_fast16_t x=0, y=0;

  COMXCoreComponent* p_omx_split = ((COMXCoreComponent**)data)[0];
  COMXCoreComponent* p_omx_resize = ((COMXCoreComponent**)data)[1];

  OMX_BUFFERHEADERTYPE *omx_buffer_fb;
  OMX_PARAM_U32TYPE singlestep_param;
  OMX_INIT_STRUCTURE(singlestep_param);
  singlestep_param.nPortIndex = COMXVideo::m_omx_split.GetOutputPort();
  singlestep_param.nU32 = 1;

  while(1){
    if(COMXVideo::m_boblight_threadstop){
      pthread_exit(0);
    }

    //set the first splitter port into the single-image mode (video goes out through the second one)
    OMX_ERRORTYPE omx_err = COMXVideo::m_omx_split.SetParameter(OMX_IndexConfigSingleStep, &singlestep_param);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - error OMX_IndexConfigSingleStep omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
    }
    
    //request a new screenshot
    omx_buffer_fb = COMXVideo::m_omx_resize.GetOutputBuffer();
    COMXVideo::m_omx_resize.FillThisBuffer(omx_buffer_fb); //the callback BufferDoneHandler will be triggered instantly

    //wait until a screenshot is available in the buffer
    pthread_mutex_lock(&COMXVideo::m_boblight_bufferdone_mutex);
    while (!COMXVideo::m_boblight_bufferdone_flag) {
      pthread_cond_wait(&COMXVideo::m_boblight_bufferdone_cond, &COMXVideo::m_boblight_bufferdone_mutex);
    }
    COMXVideo::m_boblight_bufferdone_flag = false;
    pthread_mutex_unlock(&COMXVideo::m_boblight_bufferdone_mutex);

    //process the screenshot
    if(COMXVideo::m_boblight_bufferpointer && COMXVideo::m_boblight_bufferpointer->nFilledLen != 0){
      x=COMXVideo::m_boblight_width-1;
      y=COMXVideo::m_boblight_height-1;
      //sorry for the bad readability, but using a down-counting loop helps to squeeze out some CPU cycles to reduce the boblight delay
      for(offset = (COMXVideo::m_boblight_width*COMXVideo::m_boblight_height)*4; offset>0; offset-=4, --x){
        //the buffer is filled in BGRA format -> extract RGB data boblight expects
        rgb[0] = (int)COMXVideo::m_boblight_bufferpointer->pBuffer[offset-2];
        rgb[1] = (int)COMXVideo::m_boblight_bufferpointer->pBuffer[offset-3];
        rgb[2] = (int)COMXVideo::m_boblight_bufferpointer->pBuffer[offset-4];

        boblight_addpixelxy(COMXVideo::m_boblight, x, y, rgb);

        if(x==0){
          //jump to the previous line
          x=(COMXVideo::m_boblight_width-1)+1; //1 is add to compensate the follwing decrement
          --y;
        }else{
           //check if we are in the middle of the image vertically and skip the respective horizontal parts
           if(x == COMXVideo::m_boblight_margin_r && y > COMXVideo::m_boblight_margin_t && y < COMXVideo::m_boblight_margin_b){
             x=COMXVideo::m_boblight_margin_l+1;
             offset -= (COMXVideo::m_boblight_margin_r-(COMXVideo::m_boblight_margin_l+1))*4;
           }
        }
      }
      boblight_sendrgb(COMXVideo::m_boblight, 0, NULL);
    }

    //the buffer was processed completely
    Sleep(COMXVideo::m_boblight_timeout);
  }
  pthread_exit(0); 
}


static unsigned count_bits(int32_t value)
{
  unsigned bits = 0;
  for(;value;++bits)
    value &= value - 1;
  return bits;
}

int COMXVideo::Decode(uint8_t *pData, int iSize, double dts, double pts)
{
  OMX_ERRORTYPE omx_err;

  if( m_drop_state )
    return true;

  unsigned int demuxer_bytes = (unsigned int)iSize;
  uint8_t *demuxer_content = pData;

  if (demuxer_content && demuxer_bytes > 0)
  {
    while(demuxer_bytes)
    {
      // 500ms timeout
      OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer(500);
      if(omx_buffer == NULL)
      {
        CLog::Log(LOGERROR, "OMXVideo::Decode timeout\n");
        return false;
      }

      /*
      CLog::Log(DEBUG, "COMXVideo::Video VDec : pts %lld omx_buffer 0x%08x buffer 0x%08x number %d\n", 
          pts, omx_buffer, omx_buffer->pBuffer, (int)omx_buffer->pAppPrivate);
      if(pts == DVD_NOPTS_VALUE)
      {
        CLog::Log(LOGDEBUG, "VDec : pts %f omx_buffer 0x%08x buffer 0x%08x number %d\n", 
          (float)pts / AV_TIME_BASE, (int)omx_buffer, (int)omx_buffer->pBuffer, (int)omx_buffer->pAppPrivate);
      }
      */

      omx_buffer->nFlags = 0;
      omx_buffer->nOffset = 0;
      // some packed bitstream AVI files set almost all pts values to DVD_NOPTS_VALUE, but have a scattering of real pts values.
      // the valid pts values match the dts values.
      // if a stream has had more than 4 valid pts values in the last 16, the use UNKNOWN, otherwise use dts
      m_history_valid_pts = (m_history_valid_pts << 1) | (pts != DVD_NOPTS_VALUE);
      if(pts == DVD_NOPTS_VALUE && count_bits(m_history_valid_pts & 0xffff) < 4)
        pts = dts;

      if(m_av_clock->VideoStart())
      {
        // only send dts on first frame to get nearly correct starttime
        if(pts == DVD_NOPTS_VALUE)
          pts = dts;
        omx_buffer->nFlags |= OMX_BUFFERFLAG_STARTTIME;
        CLog::Log(LOGDEBUG, "OMXVideo::Decode VDec : setStartTime %f\n", (pts == DVD_NOPTS_VALUE ? 0.0 : pts) / DVD_TIME_BASE);
        m_av_clock->VideoStart(false);
      }
      if(pts == DVD_NOPTS_VALUE)
        omx_buffer->nFlags |= OMX_BUFFERFLAG_TIME_UNKNOWN;

      omx_buffer->nTimeStamp = ToOMXTime((uint64_t)(pts == DVD_NOPTS_VALUE) ? 0 : pts);
      omx_buffer->nFilledLen = (demuxer_bytes > omx_buffer->nAllocLen) ? omx_buffer->nAllocLen : demuxer_bytes;
      memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

      demuxer_bytes -= omx_buffer->nFilledLen;
      demuxer_content += omx_buffer->nFilledLen;

      if(demuxer_bytes == 0)
        omx_buffer->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;

      int nRetry = 0;
      while(true)
      {
        omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
        if (omx_err == OMX_ErrorNone)
        {
          //CLog::Log(LOGINFO, "VideD: dts:%.0f pts:%.0f size:%d)\n", dts, pts, iSize);
          break;
        }
        else
        {
          CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
          nRetry++;
        }
        if(nRetry == 5)
        {
          CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() finaly failed\n", CLASSNAME, __func__);
          return false;
        }
      }

      omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, 0);
      if (omx_err == OMX_ErrorNone)
      {
        OMX_PARAM_PORTDEFINITIONTYPE port_image;
        OMX_INIT_STRUCTURE(port_image);
        port_image.nPortIndex = m_omx_decoder.GetOutputPort();
        omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_image);
        if(omx_err != OMX_ErrorNone)
        {
          CLog::Log(LOGERROR, "%s::%s - error m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition) omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
        }
        // let OMXPlayerVideo know about resolution so it can inform RenderManager
        if (m_res_callback)
          m_res_callback(m_res_ctx, port_image.format.video.nFrameWidth, port_image.format.video.nFrameHeight);

        m_omx_decoder.DisablePort(m_omx_decoder.GetOutputPort(), true);
        m_omx_sched.DisablePort(m_omx_sched.GetInputPort(), true);

        OMX_CONFIG_INTERLACETYPE interlace;
        OMX_INIT_STRUCTURE(interlace);
        interlace.nPortIndex = m_omx_decoder.GetOutputPort();
        omx_err = m_omx_decoder.GetConfig(OMX_IndexConfigCommonInterlace, &interlace);
        if(omx_err != OMX_ErrorNone)
        {
          CLog::Log(LOGERROR, "%s::%s - error m_omx_decoder.GetConfig(OMX_IndexConfigCommonInterlace) omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
        }

        if (m_deinterlace)
        {
          m_omx_image_fx.DisablePort(m_omx_image_fx.GetInputPort(), true);
          port_image.nPortIndex = m_omx_image_fx.GetInputPort();
          omx_err = m_omx_image_fx.SetParameter(OMX_IndexParamPortDefinition, &port_image);
          if(omx_err != OMX_ErrorNone)
          {
            CLog::Log(LOGERROR, "%s::%s - error m_omx_image_fx.SetParameter(OMX_IndexParamPortDefinition) omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
          }
          omx_err = m_omx_image_fx.WaitForEvent(OMX_EventPortSettingsChanged);
          if(omx_err != OMX_ErrorNone)
          {
             CLog::Log(LOGERROR, "%s::%s - error m_omx_image_fx.WaitForEvent(OMX_EventPortSettingsChanged) omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
          }
          port_image.nPortIndex = m_omx_image_fx.GetOutputPort();
          omx_err = m_omx_image_fx.GetParameter(OMX_IndexParamPortDefinition, &port_image);
          if(omx_err != OMX_ErrorNone)
          {
            CLog::Log(LOGERROR, "%s::%s - error m_omx_image_fx.GetParameter(OMX_IndexParamPortDefinition) omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
          }
          m_omx_image_fx.EnablePort(m_omx_image_fx.GetInputPort(), true);

          m_omx_image_fx.DisablePort(m_omx_image_fx.GetOutputPort(), true);
        }
        port_image.nPortIndex = m_omx_sched.GetInputPort();
        omx_err = m_omx_sched.SetParameter(OMX_IndexParamPortDefinition, &port_image);
        if(omx_err != OMX_ErrorNone)
        {
          CLog::Log(LOGERROR, "%s::%s - error m_omx_sched.SetParameter(OMX_IndexParamPortDefinition) omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
        }
        omx_err = m_omx_sched.WaitForEvent(OMX_EventPortSettingsChanged);
        if(omx_err != OMX_ErrorNone)
        {
           CLog::Log(LOGERROR, "%s::%s - error m_omx_sched.WaitForEvent(OMX_EventPortSettingsChanged) omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
        }
        if (m_deinterlace)
        {
          m_omx_image_fx.EnablePort(m_omx_image_fx.GetOutputPort(), true);
        }
        m_omx_decoder.EnablePort(m_omx_decoder.GetOutputPort(), true);
        m_omx_sched.EnablePort(m_omx_sched.GetInputPort(), true);
      }
    }

    return true;

  }
  
  return false;
}

void COMXVideo::Reset(void)
{
  if(!m_is_open)
    return;

  m_omx_decoder.FlushInput();
  m_omx_tunnel_decoder.Flush();

 if(COMXVideo::m_boblight){
    m_omx_split.FlushAll();
    m_omx_resize.FlushAll();
  }
  /*
  OMX_ERRORTYPE omx_err;
  OMX_CONFIG_BOOLEANTYPE configBool;
  OMX_INIT_STRUCTURE(configBool);
  configBool.bEnabled = OMX_TRUE;

  omx_err = m_omx_decoder.SetConfig(OMX_IndexConfigRefreshCodec, &configBool);
  if (omx_err != OMX_ErrorNone)
    CLog::Log(LOGERROR, "%s::%s - error reopen codec omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);

  SendDecoderConfig();

  */
}

///////////////////////////////////////////////////////////////////////////////////////////
bool COMXVideo::Pause()
{
  if(m_omx_render.GetComponent() == NULL)
    return false;

  if(m_Pause) return true;
  m_Pause = true;

  m_omx_sched.SetStateForComponent(OMX_StatePause);
  m_omx_render.SetStateForComponent(OMX_StatePause);

  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////
bool COMXVideo::Resume()
{
  if(m_omx_render.GetComponent() == NULL)
    return false;

  if(!m_Pause) return true;
  m_Pause = false;

  m_omx_sched.SetStateForComponent(OMX_StateExecuting);
  m_omx_render.SetStateForComponent(OMX_StateExecuting);

  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////
void COMXVideo::SetVideoRect(const CRect& SrcRect, const CRect& DestRect)
{
  if(!m_is_open)
    return;

  OMX_CONFIG_DISPLAYREGIONTYPE configDisplay;
  float sx1 = SrcRect.x1, sy1 = SrcRect.y1, sx2 = SrcRect.x2, sy2 = SrcRect.y2;
  float dx1 = DestRect.x1, dy1 = DestRect.y1, dx2 = DestRect.x2, dy2 = DestRect.y2;
  float sw = SrcRect.Width() / DestRect.Width();
  float sh = SrcRect.Height() / DestRect.Height();

  // doesn't like negative coordinates on dest_rect. So adjust by increasing src_rect
  if (dx1 < 0.0f) {
    sx1 -= dx1 * sw;
    dx1 = 0;
  }
  if (dy1 < 0.0f) {
    sy1 -= dy1 * sh;
    dy1 = 0;
  }

  OMX_INIT_STRUCTURE(configDisplay);
  configDisplay.nPortIndex = m_omx_render.GetInputPort();
  configDisplay.fullscreen = OMX_FALSE;
  configDisplay.noaspect   = OMX_TRUE;

  configDisplay.set                 = (OMX_DISPLAYSETTYPE)(OMX_DISPLAY_SET_DEST_RECT|OMX_DISPLAY_SET_SRC_RECT|OMX_DISPLAY_SET_FULLSCREEN|OMX_DISPLAY_SET_NOASPECT);
  configDisplay.dest_rect.x_offset  = (int)(dx1+0.5f);
  configDisplay.dest_rect.y_offset  = (int)(dy1+0.5f);
  configDisplay.dest_rect.width     = (int)(dx2-dx1+0.5f);
  configDisplay.dest_rect.height    = (int)(dy2-dy1+0.5f);

  configDisplay.src_rect.x_offset   = (int)(sx1+0.5f);
  configDisplay.src_rect.y_offset   = (int)(sy1+0.5f);
  configDisplay.src_rect.width      = (int)(sx2-sx1+0.5f);
  configDisplay.src_rect.height     = (int)(sy2-sy1+0.5f);

  m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);

  CLog::Log(LOGDEBUG, "dest_rect.x_offset %d dest_rect.y_offset %d dest_rect.width %d dest_rect.height %d\n",
      configDisplay.dest_rect.x_offset, configDisplay.dest_rect.y_offset, 
      configDisplay.dest_rect.width, configDisplay.dest_rect.height);
}

int COMXVideo::GetInputBufferSize()
{
  return m_omx_decoder.GetInputBufferSize();
}

void COMXVideo::SubmitEOS()
{
  if(!m_is_open)
    return;

  m_submitted_eos = true;

  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();
  
  if(omx_buffer == NULL)
  {
    CLog::Log(LOGERROR, "%s::%s - buffer error 0x%08x", CLASSNAME, __func__, omx_err);
    return;
  }
  
  omx_buffer->nOffset     = 0;
  omx_buffer->nFilledLen  = 0;
  omx_buffer->nTimeStamp  = ToOMXTime(0LL);

  omx_buffer->nFlags = OMX_BUFFERFLAG_ENDOFFRAME | OMX_BUFFERFLAG_EOS | OMX_BUFFERFLAG_TIME_UNKNOWN;
  
  omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
    return;
  }
}

bool COMXVideo::IsEOS()
{
  if(!m_is_open)
    return true;
  return m_omx_render.IsEOS();
}

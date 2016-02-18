/*
 * Copyright (c) 2012, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <rviz/bit_allocator.h>
#include <rviz/display_context.h>
#include <rviz/frame_manager.h>
#include <rviz/load_resource.h>
#include <rviz/ogre_helpers/axes.h>
#include <rviz/properties/display_group_visibility_property.h>
#include <rviz/properties/enum_property.h>
#include <rviz/properties/float_property.h>
#include <rviz/properties/int_property.h>
#include <rviz/properties/ros_topic_property.h>
#include <rviz/render_panel.h>
#include <rviz/uniform_string_stream.h>
#include <rviz/validate_floats.h>
#include <OgreCamera.h>
#include <OgreManualObject.h>
#include <OgreMaterialManager.h>
#include <OgreRectangle2D.h>
#include <OgreRenderSystem.h>
#include <OgreRenderWindow.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreTechnique.h>
#include <OgreTextureManager.h>
#include <OgreViewport.h>
#include <boost/bind.hpp>
#include <image_transport/camera_common.h>
#include <image_transport/image_transport.h>
#include <sensor_msgs/image_encodings.h>
#include <string>
#include <tf/transform_listener.h>

#include "rviz_camera_stream/camera_display.h"

namespace video_export
{
class VideoPublisher
{
private:
  ros::NodeHandle nh_;
  image_transport::ImageTransport it_;
  image_transport::Publisher pub_;
  uint image_id_;
public:
  VideoPublisher() :
    it_(nh_),
    image_id_(0)
  {
  }
  void shutdown()
  {
    if (pub_.getTopic() != "")
    {
      pub_.shutdown();
    }
  }

  void advertise(std::string topic)
  {
    pub_ = it_.advertise(topic, 1);
  }

  void publishFrame(Ogre::RenderWindow * render_window)
  {
    if (pub_.getTopic() == "")
    {
      return;
    }
    // RenderTarget::writeContentsToFile() used as example
    int height = render_window->getHeight();
    int width = render_window->getWidth();
    Ogre::PixelFormat pf = render_window->suggestPixelFormat();
    uint pixelsize = Ogre::PixelUtil::getNumElemBytes(pf);
    uint datasize = width * height * pixelsize;

    // 1.05 multiplier is to avoid crash when the window is resized.
    // There should be a better solution.
    uchar *data = OGRE_ALLOC_T(uchar, datasize * 1.05, Ogre::MEMCATEGORY_RENDERSYS);
    Ogre::PixelBox pb(width, height, 1, pf, data);
    render_window->copyContentsToMemory(pb);

    sensor_msgs::Image image;
    image.header.stamp = ros::Time::now();
    image.header.seq = image_id_++;
    image.height = height;
    image.width = width;
    image.step = pixelsize * width;
    image.encoding = sensor_msgs::image_encodings::RGB8;  // would break if pf changes
    image.is_bigendian = (OGRE_ENDIAN == OGRE_ENDIAN_BIG);
    image.data.resize(datasize);
    memcpy(&image.data[0], data, datasize);
    pub_.publish(image);

    OGRE_FREE(data, Ogre::MEMCATEGORY_RENDERSYS);
  }
};
}  // namespace video_export


namespace rviz
{

const QString CameraPub::BACKGROUND("background");
const QString CameraPub::OVERLAY("overlay");
const QString CameraPub::BOTH("background and overlay");

bool validateFloats(const sensor_msgs::CameraInfo& msg)
{
  bool valid = true;
  valid = valid && validateFloats(msg.D);
  valid = valid && validateFloats(msg.K);
  valid = valid && validateFloats(msg.R);
  valid = valid && validateFloats(msg.P);
  return valid;
}

CameraPub::CameraPub()
  : ImageDisplayBase()
  , texture_()
  , render_panel_(0)
  , caminfo_tf_filter_(0)
  , new_caminfo_(false)
  , force_render_(false)
  , caminfo_ok_(false)
  , video_publisher_(0)
{
  image_position_property_ = new EnumProperty("Image Rendering", BOTH,
      "Render the image behind all other geometry or overlay it on top, or both.",
      this, SLOT(forceRender()));
  image_position_property_->addOption(BACKGROUND);
  image_position_property_->addOption(OVERLAY);
  image_position_property_->addOption(BOTH);
}

CameraPub::~CameraPub()
{
  if (initialized())
  {
    render_panel_->getRenderWindow()->removeListener(this);

    unsubscribe();
    caminfo_tf_filter_->clear();


    // workaround. delete results in a later crash
    render_panel_->hide();
    // delete render_panel_;

    delete bg_screen_rect_;
    delete fg_screen_rect_;

    bg_scene_node_->getParentSceneNode()->removeAndDestroyChild(bg_scene_node_->getName());
    fg_scene_node_->getParentSceneNode()->removeAndDestroyChild(fg_scene_node_->getName());

    delete caminfo_tf_filter_;

    context_->visibilityBits()->freeBits(vis_bit_);
  }
}

void CameraPub::onInitialize()
{
  ImageDisplayBase::onInitialize();

  video_publisher_ = new video_export::VideoPublisher();

  caminfo_tf_filter_ = new tf::MessageFilter<sensor_msgs::CameraInfo>(
      *context_->getTFClient(), fixed_frame_.toStdString(),
      queue_size_property_->getInt(), update_nh_);

  bg_scene_node_ = scene_node_->createChildSceneNode();
  fg_scene_node_ = scene_node_->createChildSceneNode();

  {
    static int count = 0;
    UniformStringStream ss;
    ss << "CameraPubObject" << count++;

    // background rectangle
    bg_screen_rect_ = new Ogre::Rectangle2D(true);
    bg_screen_rect_->setCorners(-1.0f, 1.0f, 1.0f, -1.0f);

    ss << "Material";
    bg_material_ = Ogre::MaterialManager::getSingleton().create(
        ss.str(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    bg_material_->setDepthWriteEnabled(false);

    bg_material_->setReceiveShadows(false);
    bg_material_->setDepthCheckEnabled(false);

    bg_material_->getTechnique(0)->setLightingEnabled(false);
    Ogre::TextureUnitState* tu = bg_material_->getTechnique(0)->getPass(0)->createTextureUnitState();
    tu->setTextureName(texture_.getTexture()->getName());
    tu->setTextureFiltering(Ogre::TFO_NONE);
    tu->setAlphaOperation(Ogre::LBX_SOURCE1, Ogre::LBS_MANUAL, Ogre::LBS_CURRENT, 0.0);

    bg_material_->setCullingMode(Ogre::CULL_NONE);
    bg_material_->setSceneBlending(Ogre::SBT_REPLACE);

    Ogre::AxisAlignedBox aabInf;
    aabInf.setInfinite();

    bg_screen_rect_->setRenderQueueGroup(Ogre::RENDER_QUEUE_BACKGROUND);
    bg_screen_rect_->setBoundingBox(aabInf);
    bg_screen_rect_->setMaterial(bg_material_->getName());

    bg_scene_node_->attachObject(bg_screen_rect_);
    bg_scene_node_->setVisible(false);

    // overlay rectangle
    fg_screen_rect_ = new Ogre::Rectangle2D(true);
    fg_screen_rect_->setCorners(-1.0f, 1.0f, 1.0f, -1.0f);

    fg_material_ = bg_material_->clone(ss.str() + "fg");
    fg_screen_rect_->setBoundingBox(aabInf);
    fg_screen_rect_->setMaterial(fg_material_->getName());

    fg_material_->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    fg_screen_rect_->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);

    fg_scene_node_->attachObject(fg_screen_rect_);
    fg_scene_node_->setVisible(false);
  }

  updateAlpha();

  render_panel_ = new RenderPanel();
  render_panel_->getRenderWindow()->addListener(this);
  render_panel_->getRenderWindow()->setAutoUpdated(false);
  render_panel_->getRenderWindow()->setActive(false);
  render_panel_->resize(640, 480);
  render_panel_->initialize(context_->getSceneManager(), context_);

  setAssociatedWidget(render_panel_);

  render_panel_->setAutoRender(false);
  render_panel_->setOverlaysEnabled(false);
  render_panel_->getCamera()->setNearClipDistance(0.01f);

  caminfo_tf_filter_->connectInput(caminfo_sub_);
  caminfo_tf_filter_->registerCallback(boost::bind(&CameraPub::caminfoCallback, this, _1));
  // context_->getFrameManager()->registerFilterForTransformStatusCheck(caminfo_tf_filter_, this);

  vis_bit_ = context_->visibilityBits()->allocBit();
  render_panel_->getViewport()->setVisibilityMask(vis_bit_);

  visibility_property_ = new DisplayGroupVisibilityProperty(
    vis_bit_, context_->getRootDisplayGroup(), this, "Visibility", true,
    "Changes the visibility of other Displays in the camera view.");

  visibility_property_->setIcon(loadPixmap("package://rviz/icons/visibility.svg", true));

  this->addChild(visibility_property_, 0);
}

void CameraPub::preRenderTargetUpdate(const Ogre::RenderTargetEvent& evt)
{
  QString image_position = image_position_property_->getString();
  bg_scene_node_->setVisible(caminfo_ok_ && (image_position == BACKGROUND || image_position == BOTH));
  fg_scene_node_->setVisible(caminfo_ok_ && (image_position == OVERLAY || image_position == BOTH));

  // set view flags on all displays
  visibility_property_->update();
}

void CameraPub::postRenderTargetUpdate(const Ogre::RenderTargetEvent& evt)
{
  bg_scene_node_->setVisible(false);
  fg_scene_node_->setVisible(false);

  // Publish the rendered window video stream
  video_publisher_->publishFrame(render_panel_->getRenderWindow());
}

void CameraPub::onEnable()
{
  subscribe();
  render_panel_->getRenderWindow()->setActive(true);
}

void CameraPub::onDisable()
{
  render_panel_->getRenderWindow()->setActive(false);
  unsubscribe();
  clear();
}

void CameraPub::subscribe()
{
  if ((!isEnabled()) || (topic_property_->getTopicStd().empty()))
  {
    return;
  }

  std::string target_frame = fixed_frame_.toStdString();
  ImageDisplayBase::enableTFFilter(target_frame);

  ImageDisplayBase::subscribe();

  std::string topic = topic_property_->getTopicStd();
  std::string caminfo_topic = image_transport::getCameraInfoTopic(topic_property_->getTopicStd());

  try
  {
    caminfo_sub_.subscribe(update_nh_, caminfo_topic, 1);
    setStatus(StatusProperty::Ok, "Camera Info", "OK");
  }
  catch (ros::Exception& e)
  {
    setStatus(StatusProperty::Error, "Camera Info", QString("Error subscribing: ") + e.what());
  }

  // TODO(lwalter) need to make this topic come from plugin ui
  video_publisher_->advertise("rviz_out");
}

void CameraPub::unsubscribe()
{
  video_publisher_->shutdown();
  ImageDisplayBase::unsubscribe();
  caminfo_sub_.unsubscribe();
}

void CameraPub::updateAlpha()
{
  Ogre::Pass* pass = fg_material_->getTechnique(0)->getPass(0);
  if (pass->getNumTextureUnitStates() > 0)
  {
    Ogre::TextureUnitState* tex_unit = pass->getTextureUnitState(0);
    tex_unit->setAlphaOperation(Ogre::LBX_MODULATE, Ogre::LBS_MANUAL, Ogre::LBS_CURRENT, 1.0);
  }
  else
  {
    fg_material_->setAmbient(Ogre::ColourValue(0.0f, 1.0f, 1.0f, 1.0f));
    fg_material_->setDiffuse(Ogre::ColourValue(0.0f, 1.0f, 1.0f, 1.0f));
  }

  force_render_ = true;
  context_->queueRender();
}

void CameraPub::forceRender()
{
  force_render_ = true;
  context_->queueRender();
}

void CameraPub::updateQueueSize()
{
  caminfo_tf_filter_->setQueueSize((uint32_t) queue_size_property_->getInt());
  ImageDisplayBase::updateQueueSize();
}

void CameraPub::clear()
{
  texture_.clear();
  force_render_ = true;
  context_->queueRender();

  new_caminfo_ = false;
  current_caminfo_.reset();

  setStatus(StatusProperty::Warn, "Camera Info",
            "No CameraInfo received on [" +
            QString::fromStdString(caminfo_sub_.getTopic()) +
            "].  Topic may not exist.");
  setStatus(StatusProperty::Warn, "Image", "No Image received");

  render_panel_->getCamera()->setPosition(Ogre::Vector3(999999, 999999, 999999));
}

void CameraPub::update(float wall_dt, float ros_dt)
{
  try
  {
    if (texture_.update() || force_render_)
    {
      caminfo_ok_ = updateCamera();
      force_render_ = false;
    }
  }
  catch (UnsupportedImageEncoding& e)
  {
    setStatus(StatusProperty::Error, "Image", e.what());
  }

  render_panel_->getRenderWindow()->update();
}

bool CameraPub::updateCamera()
{
  sensor_msgs::CameraInfo::ConstPtr info;
  sensor_msgs::Image::ConstPtr image;
  {
    boost::mutex::scoped_lock lock(caminfo_mutex_);

    info = current_caminfo_;
    image = texture_.getImage();
  }

  if (!info || !image)
  {
    return false;
  }

  if (!validateFloats(*info))
  {
    setStatus(StatusProperty::Error, "Camera Info", "Contains invalid floating point values (nans or infs)");
    return false;
  }

  // if we're in 'exact' time mode, only show image if the time is exactly right
  ros::Time rviz_time = context_->getFrameManager()->getTime();
  if (context_->getFrameManager()->getSyncMode() == FrameManager::SyncExact &&
      rviz_time != image->header.stamp)
  {
    std::ostringstream s;
    s << "Time-syncing active and no image at timestamp " << rviz_time.toSec() << ".";
    setStatus(StatusProperty::Warn, "Time", s.str().c_str());
    return false;
  }

  Ogre::Vector3 position;
  Ogre::Quaternion orientation;
  context_->getFrameManager()->getTransform(image->header.frame_id, image->header.stamp, position, orientation);

  // printf( "CameraPub:updateCamera(): pos = %.2f, %.2f, %.2f.\n", position.x, position.y, position.z );

  // convert vision (Z-forward) frame to ogre frame (Z-out)
  orientation = orientation * Ogre::Quaternion(Ogre::Degree(180), Ogre::Vector3::UNIT_X);

  float img_width = info->width;
  float img_height = info->height;

  // If the image width is 0 due to a malformed caminfo, try to grab the width from the image.
  if (img_width == 0)
  {
    ROS_DEBUG("Malformed CameraInfo on camera [%s], width = 0", qPrintable(getName()));
    img_width = texture_.getWidth();
  }

  if (img_height == 0)
  {
    ROS_DEBUG("Malformed CameraInfo on camera [%s], height = 0", qPrintable(getName()));
    img_height = texture_.getHeight();
  }

  if (img_height == 0.0 || img_width == 0.0)
  {
    setStatus(StatusProperty::Error, "Camera Info",
              "Could not determine width/height of image due to malformed CameraInfo (either width or height is 0)");
    return false;
  }

  double fx = info->P[0];
  double fy = info->P[5];

  float win_width = render_panel_->width();
  float win_height = render_panel_->height();
  float zoom_x = 1.0;
  float zoom_y = zoom_x;

  // Preserve aspect ratio
  if (win_width != 0 && win_height != 0)
  {
    float img_aspect = (img_width / fx) / (img_height / fy);
    float win_aspect = win_width / win_height;

    if (img_aspect > win_aspect)
    {
      zoom_y = zoom_y / img_aspect * win_aspect;
    }
    else
    {
      zoom_x = zoom_x / win_aspect * img_aspect;
    }
  }

  // Add the camera's translation relative to the left camera (from P[3]);
  double tx = -1 * (info->P[3] / fx);
  Ogre::Vector3 right = orientation * Ogre::Vector3::UNIT_X;
  position = position + (right * tx);

  double ty = -1 * (info->P[7] / fy);
  Ogre::Vector3 down = orientation * Ogre::Vector3::UNIT_Y;
  position = position + (down * ty);

  if (!validateFloats(position))
  {
    setStatus(StatusProperty::Error, "Camera Info",
        "CameraInfo/P resulted in an invalid position calculation (nans or infs)");
    return false;
  }

  render_panel_->getCamera()->setPosition(position);
  render_panel_->getCamera()->setOrientation(orientation);

  // calculate the projection matrix
  double cx = info->P[2];
  double cy = info->P[6];

  double far_plane = 100;
  double near_plane = 0.01;

  Ogre::Matrix4 proj_matrix;
  proj_matrix = Ogre::Matrix4::ZERO;

  proj_matrix[0][0] = 2.0 * fx / img_width * zoom_x;
  proj_matrix[1][1] = 2.0 * fy / img_height * zoom_y;

  proj_matrix[0][2] = 2.0 * (0.5 - cx / img_width) * zoom_x;
  proj_matrix[1][2] = 2.0 * (cy / img_height - 0.5) * zoom_y;

  proj_matrix[2][2] = -(far_plane + near_plane) / (far_plane - near_plane);
  proj_matrix[2][3] = -2.0 * far_plane * near_plane / (far_plane - near_plane);

  proj_matrix[3][2] = -1;

  render_panel_->getCamera()->setCustomProjectionMatrix(true, proj_matrix);

  setStatus(StatusProperty::Ok, "Camera Info", "OK");

#if 0
  static Axes* debug_axes = new Axes(scene_manager_, 0, 0.2, 0.01);
  debug_axes->setPosition(position);
  debug_axes->setOrientation(orientation);
#endif

  // adjust the image rectangles to fit the zoom & aspect ratio
  bg_screen_rect_->setCorners(-1.0f * zoom_x, 1.0f * zoom_y, 1.0f * zoom_x, -1.0f * zoom_y);
  fg_screen_rect_->setCorners(-1.0f * zoom_x, 1.0f * zoom_y, 1.0f * zoom_x, -1.0f * zoom_y);

  Ogre::AxisAlignedBox aabInf;
  aabInf.setInfinite();
  bg_screen_rect_->setBoundingBox(aabInf);
  fg_screen_rect_->setBoundingBox(aabInf);

  setStatus(StatusProperty::Ok, "Time", "ok");
  setStatus(StatusProperty::Ok, "Camera Info", "ok");

  return true;
}

void CameraPub::processMessage(const sensor_msgs::Image::ConstPtr& msg)
{
  texture_.addMessage(msg);
}

void CameraPub::caminfoCallback(const sensor_msgs::CameraInfo::ConstPtr& msg)
{
  boost::mutex::scoped_lock lock(caminfo_mutex_);
  current_caminfo_ = msg;
  new_caminfo_ = true;
}

void CameraPub::fixedFrameChanged()
{
  std::string targetFrame = fixed_frame_.toStdString();
  caminfo_tf_filter_->setTargetFrame(targetFrame);
  ImageDisplayBase::fixedFrameChanged();
}

void CameraPub::reset()
{
  ImageDisplayBase::reset();
  clear();
}

}  // namespace rviz

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(rviz::CameraPub, rviz::Display)

// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_COLOR_SOURCE_CONTENTS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_COLOR_SOURCE_CONTENTS_H_

#include "fml/logging.h"
#include "impeller/entity/contents/clip_contents.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/contents.h"
#include "impeller/entity/geometry/geometry.h"
#include "impeller/entity/geometry/rect_geometry.h"
#include "impeller/geometry/matrix.h"

namespace impeller {

//------------------------------------------------------------------------------
/// Color sources are geometry-ignostic `Contents` capable of shading any area
/// defined by an `impeller::Geometry`. Conceptually,
/// `impeller::ColorSourceContents` implement a particular color shading
/// behavior.
///
/// This separation of concerns between geometry and color source output allows
/// Impeller to handle most possible draw combinations in a consistent way.
/// For example: There are color sources for handling solid colors, gradients,
/// textures, custom runtime effects, and even 3D scenes.
///
/// There are some special rendering exceptions that deviate from this pattern
/// and cross geometry and color source concerns, such as text atlas and image
/// atlas rendering. Special `Contents` exist for rendering these behaviors
/// which don't implement `ColorSourceContents`.
///
/// @see  `impeller::Geometry`
///
class ColorSourceContents : public Contents {
 public:
  ColorSourceContents();

  ~ColorSourceContents() override;

  //----------------------------------------------------------------------------
  /// @brief  Set the geometry that this contents will use to render.
  ///
  void SetGeometry(std::shared_ptr<Geometry> geometry);

  //----------------------------------------------------------------------------
  /// @brief  Get the geometry that this contents will use to render.
  ///
  const std::shared_ptr<Geometry>& GetGeometry() const;

  //----------------------------------------------------------------------------
  /// @brief  Set the effect transform for this color source.
  ///
  ///         The effect transform is a transform matrix that is applied to
  ///         the shaded color output and does not impact geometry in any way.
  ///
  ///         For example: With repeat tiling, any gradient or
  ///         `TiledTextureContents` could be used with an effect transform to
  ///         inexpensively draw an infinite scrolling background pattern.
  ///
  void SetEffectTransform(Matrix matrix);

  //----------------------------------------------------------------------------
  /// @brief   Set the inverted effect transform for this color source.
  ///
  ///          When the effect transform is set via `SetEffectTransform`, the
  ///          value is inverted upon storage. The reason for this is that most
  ///          color sources internally use the inverted transform.
  ///
  /// @return  The inverse of the transform set by `SetEffectTransform`.
  ///
  /// @see     `SetEffectTransform`
  ///
  const Matrix& GetInverseEffectTransform() const;

  //----------------------------------------------------------------------------
  /// @brief  Set the opacity factor for this color source.
  ///
  void SetOpacityFactor(Scalar opacity);

  //----------------------------------------------------------------------------
  /// @brief  Get the opacity factor for this color source.
  ///
  ///         This value is is factored into the output of the color source in
  ///         addition to opacity information that may be supplied any other
  ///         inputs.
  ///
  /// @note   If set, the output of this method factors factors in the inherited
  ///         opacity of this `Contents`.
  ///
  /// @see    `Contents::CanInheritOpacity`
  ///
  Scalar GetOpacityFactor() const;

  virtual bool IsSolidColor() const;

  // |Contents|
  std::optional<Rect> GetCoverage(const Entity& entity) const override;

  // |Contents|
  bool CanInheritOpacity(const Entity& entity) const override;

  // |Contents|
  void SetInheritedOpacity(Scalar opacity) override;

 protected:
  using BindFragmentCallback = std::function<bool(RenderPass& pass)>;
  using PipelineBuilderMethod = std::shared_ptr<Pipeline<PipelineDescriptor>> (
      impeller::ContentContext::*)(ContentContextOptions) const;
  using PipelineBuilderCallback =
      std::function<std::shared_ptr<Pipeline<PipelineDescriptor>>(
          ContentContextOptions)>;

  template <typename VertexShaderT>
  bool DrawGeometry(const ContentContext& renderer,
                    const Entity& entity,
                    RenderPass& pass,
                    const PipelineBuilderCallback& pipeline_callback,
                    typename VertexShaderT::FrameInfo frame_info,
                    const BindFragmentCallback& bind_fragment_callback) const {
    auto options = OptionsFromPassAndEntity(pass, entity);

    GeometryResult::Mode geometry_mode = GetGeometry()->GetResultMode();
    Geometry& geometry = *GetGeometry();

    const bool is_stencil_then_cover =
        geometry_mode == GeometryResult::Mode::kNonZero ||
        geometry_mode == GeometryResult::Mode::kEvenOdd;
    if (is_stencil_then_cover) {
      pass.SetStencilReference(0);

      /// Stencil preparation draw.

      GeometryResult stencil_geometry_result =
          GetGeometry()->GetPositionBuffer(renderer, entity, pass);
      if (stencil_geometry_result.vertex_buffer.vertex_count == 0u) {
        return true;
      }
      pass.SetVertexBuffer(std::move(stencil_geometry_result.vertex_buffer));
      options.primitive_type = stencil_geometry_result.type;

      options.blend_mode = BlendMode::kDestination;
      switch (stencil_geometry_result.mode) {
        case GeometryResult::Mode::kNonZero:
          pass.SetCommandLabel("Stencil preparation (NonZero)");
          options.stencil_mode =
              ContentContextOptions::StencilMode::kStencilNonZeroFill;
          break;
        case GeometryResult::Mode::kEvenOdd:
          pass.SetCommandLabel("Stencil preparation (EvenOdd)");
          options.stencil_mode =
              ContentContextOptions::StencilMode::kStencilEvenOddFill;
          break;
        default:
          FML_UNREACHABLE();
      }
      pass.SetPipeline(renderer.GetClipPipeline(options));
      ClipPipeline::VertexShader::FrameInfo clip_frame_info;
      clip_frame_info.depth = entity.GetShaderClipDepth();
      clip_frame_info.mvp = stencil_geometry_result.transform;
      ClipPipeline::VertexShader::BindFrameInfo(
          pass, renderer.GetTransientsBuffer().EmplaceUniform(clip_frame_info));

      if (!pass.Draw().ok()) {
        return false;
      }

      /// Cover draw.

      options.blend_mode = entity.GetBlendMode();
      options.stencil_mode = ContentContextOptions::StencilMode::kCoverCompare;
      std::optional<Rect> maybe_cover_area = GetGeometry()->GetCoverage({});
      if (!maybe_cover_area.has_value()) {
        return true;
      }
      geometry = RectGeometry(maybe_cover_area.value());
    }

    GeometryResult geometry_result =
        geometry.GetPositionBuffer(renderer, entity, pass);
    if (geometry_result.vertex_buffer.vertex_count == 0u) {
      return true;
    }
    pass.SetVertexBuffer(std::move(geometry_result.vertex_buffer));
    options.primitive_type = geometry_result.type;

    // Take the pre-populated vertex shader uniform struct and set managed
    // values.
    frame_info.mvp = geometry_result.transform;

    // If overdraw prevention is enabled (like when drawing stroke paths), we
    // increment the stencil buffer as we draw, preventing overlapping fragments
    // from drawing. Afterwards, we need to append another draw call to clean up
    // the stencil buffer (happens below in this method).
    if (geometry_result.mode == GeometryResult::Mode::kPreventOverdraw) {
      options.stencil_mode =
          ContentContextOptions::StencilMode::kOverdrawPreventionIncrement;
    }
    pass.SetStencilReference(0);

    VertexShaderT::BindFrameInfo(
        pass, renderer.GetTransientsBuffer().EmplaceUniform(frame_info));

    // The reason we need to have a callback mechanism here is that this routine
    // may insert draw calls before the main draw call below. For example, for
    // sufficiently complex paths we may opt to use stencil-then-cover to avoid
    // tessellation.
    if (!bind_fragment_callback(pass)) {
      return false;
    }

    pass.SetPipeline(pipeline_callback(options));

    if (!pass.Draw().ok()) {
      return false;
    }

    // If we performed overdraw prevention, a subsection of the clip heightmap
    // was incremented by 1 in order to self-clip. So simply append a clip
    // restore to clean it up.
    if (geometry_result.mode == GeometryResult::Mode::kPreventOverdraw) {
      auto restore = ClipRestoreContents();
      restore.SetRestoreCoverage(GetCoverage(entity));
      Entity restore_entity = entity.Clone();
      return restore.Render(renderer, restore_entity, pass);
    }
    return true;
  }

 private:
  std::shared_ptr<Geometry> geometry_;
  Matrix inverse_matrix_;
  Scalar opacity_ = 1.0;
  Scalar inherited_opacity_ = 1.0;

  ColorSourceContents(const ColorSourceContents&) = delete;

  ColorSourceContents& operator=(const ColorSourceContents&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_COLOR_SOURCE_CONTENTS_H_

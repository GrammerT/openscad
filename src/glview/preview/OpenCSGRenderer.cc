/*
 *  OpenSCAD (www.openscad.org)
 *  Copyright (C) 2009-2011 Clifford Wolf <clifford@clifford.at> and
 *                          Marius Kintel <marius@kintel.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  As a special exception, you have permission to link this program
 *  with the CGAL library and distribute executables, as long as you
 *  follow the requirements of the GNU GPL in regard to all of the
 *  software in the executable aside from CGAL.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "system-gl.h"
#include "memory.h"
#include "OpenCSGRenderer.h"

#include <utility>
#include "PolySet.h"
#include "Feature.h"
#include "VertexStateManager.h"

#ifdef ENABLE_OPENCSG

class OpenCSGPrim : public OpenCSG::Primitive
{
public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  OpenCSGPrim(OpenCSG::Operation operation, unsigned int convexity, const OpenCSGRenderer& renderer) :
    OpenCSG::Primitive(operation, convexity), renderer(renderer) { }
  std::shared_ptr<const PolySet> geom;
  Transform3d m;
  Renderer::csgmode_e csgmode{Renderer::CSGMODE_NONE};

  void render() override {
    if (geom) {
      glPushMatrix();
      glMultMatrixd(m.data());
      renderer.render_surface(*geom, csgmode, m);
      glPopMatrix();
    }
  }
private:
  const OpenCSGRenderer& renderer;
};

class OpenCSGVBOPrim : public OpenCSG::Primitive
{
public:
  OpenCSGVBOPrim(OpenCSG::Operation operation, unsigned int convexity, std::unique_ptr<VertexState> vertex_state) :
    OpenCSG::Primitive(operation, convexity), vertex_state(std::move(vertex_state)) { }
  void render() override {
    if (vertex_state != nullptr) {
      vertex_state->draw();
    } else {
      if (OpenSCAD::debug != "") PRINTD("OpenCSGVBOPrim vertex_state was null");
    }
  }

private:
  const std::unique_ptr<VertexState> vertex_state;
};
#endif // ENABLE_OPENCSG

OpenCSGRenderer::OpenCSGRenderer(std::shared_ptr<CSGProducts> root_products,
                                 std::shared_ptr<CSGProducts> highlights_products,
                                 std::shared_ptr<CSGProducts> background_products)
  : root_products(std::move(root_products)),
  highlights_products(std::move(highlights_products)),
  background_products(std::move(background_products))
{
}

void OpenCSGRenderer::prepare(bool /*showfaces*/, bool /*showedges*/, const shaderinfo_t *shaderinfo)
{
  if (Feature::ExperimentalVxORenderers.is_enabled() && !vbo_vertex_products.size()) {
    if (this->root_products) {
      createCSGProducts(*this->root_products, shaderinfo, false, false);
    }
    if (this->background_products) {
      createCSGProducts(*this->background_products, shaderinfo, false, true);
    }
    if (this->highlights_products) {
      createCSGProducts(*this->highlights_products, shaderinfo, true, false);
    }
  }
}

void OpenCSGRenderer::draw(bool /*showfaces*/, bool showedges, const shaderinfo_t *shaderinfo) const
{
  if (!shaderinfo && showedges) shaderinfo = &getShader();

  if (!Feature::ExperimentalVxORenderers.is_enabled()) {
    if (this->root_products) {
      renderCSGProducts(this->root_products, showedges, shaderinfo, false, false);
    }
    if (this->background_products) {
      renderCSGProducts(this->background_products, showedges, shaderinfo, false, true);
    }
    if (this->highlights_products) {
      renderCSGProducts(this->highlights_products, showedges, shaderinfo, true, false);
    }
  } else {
    renderCSGProducts(std::make_shared<CSGProducts>(), showedges, shaderinfo);
  }
}

// Primitive for rendering using OpenCSG
OpenCSGPrim *OpenCSGRenderer::createCSGPrimitive(const CSGChainObject& csgobj, OpenCSG::Operation operation, bool highlight_mode, bool background_mode, OpenSCADOperator type) const
{
  auto *prim = new OpenCSGPrim(operation, csgobj.leaf->geom->getConvexity(), *this);
  std::shared_ptr<const PolySet> ps = dynamic_pointer_cast<const PolySet>(csgobj.leaf->geom);
  if (ps) {
    prim->geom = ps;
  }
  prim->m = csgobj.leaf->matrix;
  prim->csgmode = get_csgmode(highlight_mode, background_mode, type);
  return prim;
}

// Primitive for drawing using OpenCSG
OpenCSGVBOPrim *OpenCSGRenderer::createVBOPrimitive(const std::shared_ptr<OpenCSGVertexState>& vertex_state,
                                                    const OpenCSG::Operation operation, const unsigned int convexity) const
{
  std::unique_ptr<VertexState> opencsg_vs = std::make_unique<VertexState>(vertex_state->drawMode(), vertex_state->drawSize(),
                                                                          vertex_state->drawType(), vertex_state->drawOffset(),
                                                                          vertex_state->elementOffset(), vertex_state->verticesVBO(),
                                                                          vertex_state->elementsVBO());
  // First two glBegin entries are the vertex position calls
  opencsg_vs->glBegin().insert(opencsg_vs->glBegin().begin(), vertex_state->glBegin().begin(), vertex_state->glBegin().begin() + 2);
  // First glEnd entry is the disable vertex position call
  opencsg_vs->glEnd().insert(opencsg_vs->glEnd().begin(), vertex_state->glEnd().begin(), vertex_state->glEnd().begin() + 1);

  return new OpenCSGVBOPrim(operation, convexity, std::move(opencsg_vs));
}

void OpenCSGRenderer::createCSGProducts(const CSGProducts& products, const Renderer::shaderinfo_t * /*shaderinfo*/, bool highlight_mode, bool background_mode)
{
  size_t vbo_count = products.products.size();
  if (vbo_count) {
    if (Feature::ExperimentalVxORenderersIndexing.is_enabled()) {
      vbo_count *= 2;
    }
    all_vbos_.resize(vbo_count);
    glGenBuffers(vbo_count, all_vbos_.data());
  }

#ifdef ENABLE_OPENCSG
  size_t vbo_index = 0;
  for (const auto& product : products.products) {
    Color4f last_color;
    std::unique_ptr<OpenCSGPrimitives> primitives = std::make_unique<OpenCSGPrimitives>();
    std::unique_ptr<VertexStates> vertex_states = std::make_unique<VertexStates>();
    VertexArray vertex_array(std::make_shared<OpenCSGVertexStateFactory>(), *(vertex_states.get()),
                             all_vbos_[vbo_index++]);
    VertexStateManager vsm(*this, vertex_array);
    vertex_array.addSurfaceData();
    vertex_array.writeSurface();
    add_shader_data(vertex_array);

    size_t num_vertices = 0;
    for (const auto& csgobj : product.intersections) {
      if (csgobj.leaf->geom) {
        num_vertices += getSurfaceBufferSize(csgobj, highlight_mode, background_mode, OpenSCADOperator::INTERSECTION);
      }
    }
    for (const auto& csgobj : product.subtractions) {
      if (csgobj.leaf->geom) {
        num_vertices += getSurfaceBufferSize(csgobj, highlight_mode, background_mode, OpenSCADOperator::DIFFERENCE);
      }
    }

    vsm.initializeSize(num_vertices, all_vbos_, vbo_index);

    for (const auto& csgobj : product.intersections) {
      if (csgobj.leaf->geom) {
        const auto *ps = dynamic_cast<const PolySet *>(csgobj.leaf->geom.get());
        if (!ps) continue;

        const Color4f& c = csgobj.leaf->color;
        csgmode_e csgmode = get_csgmode(highlight_mode, background_mode);

        ColorMode colormode = ColorMode::NONE;
        if (highlight_mode) {
          colormode = ColorMode::HIGHLIGHT;
        } else if (background_mode) {
          colormode = ColorMode::BACKGROUND;
        } else {
          colormode = ColorMode::MATERIAL;
        }

        Color4f color;
        if (getShaderColor(colormode, c, color)) {
          last_color = color;
        }

        vsm.addColor(last_color);

        if (color[3] == 1.0f) {
          // object is opaque, draw normally
          create_surface(*ps, vertex_array, csgmode, csgobj.leaf->matrix, last_color);
          std::shared_ptr<OpenCSGVertexState> surface = std::dynamic_pointer_cast<OpenCSGVertexState>(vertex_states->back());
          if (surface != nullptr) {
            surface->csgObjectIndex(csgobj.leaf->index);
            primitives->emplace_back(createVBOPrimitive(surface,
                                                        OpenCSG::Intersection,
                                                        csgobj.leaf->geom->getConvexity()));
          }
        } else {
          // object is transparent, so draw rear faces first.  Issue #1496
          std::shared_ptr<VertexState> cull = std::make_shared<VertexState>();
          cull->glBegin().emplace_back([]() {
            GL_TRACE0("glEnable(GL_CULL_FACE)"); glEnable(GL_CULL_FACE);
          });
          cull->glBegin().emplace_back([]() {
            GL_TRACE0("glCullFace(GL_FRONT)"); glCullFace(GL_FRONT);
          });
          vertex_states->emplace_back(std::move(cull));

          create_surface(*ps, vertex_array, csgmode, csgobj.leaf->matrix, last_color);
          std::shared_ptr<OpenCSGVertexState> surface = std::dynamic_pointer_cast<OpenCSGVertexState>(vertex_states->back());

          if (surface != nullptr) {
            surface->csgObjectIndex(csgobj.leaf->index);

            primitives->emplace_back(createVBOPrimitive(surface,
                                                        OpenCSG::Intersection,
                                                        csgobj.leaf->geom->getConvexity()));

            cull = std::make_shared<VertexState>();
            cull->glBegin().emplace_back([]() {
              GL_TRACE0("glCullFace(GL_BACK)"); glCullFace(GL_BACK);
            });
            vertex_states->emplace_back(std::move(cull));

            vertex_states->emplace_back(surface);

            cull = std::make_shared<VertexState>();
            cull->glEnd().emplace_back([]() {
              GL_TRACE0("glDisable(GL_CULL_FACE)"); glDisable(GL_CULL_FACE);
            });
            vertex_states->emplace_back(std::move(cull));
          } else {
            assert(false && "Intersection surface state was nullptr");
          }
        }
      }
    }

    for (const auto& csgobj : product.subtractions) {
      if (csgobj.leaf->geom) {
        const auto *ps = dynamic_cast<const PolySet *>(csgobj.leaf->geom.get());
        if (!ps) continue;
        const Color4f& c = csgobj.leaf->color;
        csgmode_e csgmode = get_csgmode(highlight_mode, background_mode, OpenSCADOperator::DIFFERENCE);

        ColorMode colormode = ColorMode::NONE;
        if (highlight_mode) {
          colormode = ColorMode::HIGHLIGHT;
        } else if (background_mode) {
          colormode = ColorMode::BACKGROUND;
        } else {
          colormode = ColorMode::CUTOUT;
        }

        Color4f color;
        if (getShaderColor(colormode, c, color)) {
          last_color = color;
        }

        vsm.addColor(last_color);

        // negative objects should only render rear faces
        std::shared_ptr<VertexState> cull = std::make_shared<VertexState>();
        cull->glBegin().emplace_back([]() {
          GL_TRACE0("glEnable(GL_CULL_FACE)");
          GL_CHECKD(glEnable(GL_CULL_FACE));
        });
        cull->glBegin().emplace_back([]() {
          GL_TRACE0("glCullFace(GL_FRONT)");
          GL_CHECKD(glCullFace(GL_FRONT));
        });
        vertex_states->emplace_back(std::move(cull));

        create_surface(*ps, vertex_array, csgmode, csgobj.leaf->matrix, last_color);
        std::shared_ptr<OpenCSGVertexState> surface = std::dynamic_pointer_cast<OpenCSGVertexState>(vertex_states->back());
        if (surface != nullptr) {
          surface->csgObjectIndex(csgobj.leaf->index);
          primitives->emplace_back(createVBOPrimitive(surface,
                                                      OpenCSG::Subtraction,
                                                      csgobj.leaf->geom->getConvexity()));
        } else {
          assert(false && "Subtraction surface state was nullptr");
        }

        cull = std::make_shared<VertexState>();
        cull->glEnd().emplace_back([]() {
          GL_TRACE0("glDisable(GL_CULL_FACE)");
          GL_CHECKD(glDisable(GL_CULL_FACE));
        });
        vertex_states->emplace_back(std::move(cull));
      }
    }

    if (Feature::ExperimentalVxORenderersDirect.is_enabled() || Feature::ExperimentalVxORenderersPrealloc.is_enabled()) {
      if (Feature::ExperimentalVxORenderersIndexing.is_enabled()) {
        GL_TRACE0("glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)");
        GL_CHECKD(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
      }
      GL_TRACE0("glBindBuffer(GL_ARRAY_BUFFER, 0)");
      GL_CHECKD(glBindBuffer(GL_ARRAY_BUFFER, 0));
    }

    vertex_array.createInterleavedVBOs();
    vbo_vertex_products.emplace_back(std::make_unique<OpenCSGVBOProduct>(
                                       std::move(primitives), std::move(vertex_states)));
  }
#endif // ENABLE_OPENCSG
}

void OpenCSGRenderer::renderCSGProducts(const std::shared_ptr<CSGProducts>& products, bool showedges,
                                        const Renderer::shaderinfo_t *shaderinfo,
                                        bool highlight_mode, bool background_mode) const
{
#ifdef ENABLE_OPENCSG
  if (!Feature::ExperimentalVxORenderers.is_enabled()) {
    for (const auto& product : products->products) {
      std::vector<OpenCSG::Primitive *> primitives;
      for (const auto& csgobj : product.intersections) {
        if (csgobj.leaf->geom) primitives.push_back(createCSGPrimitive(csgobj, OpenCSG::Intersection, highlight_mode, background_mode, OpenSCADOperator::INTERSECTION));
      }
      for (const auto& csgobj : product.subtractions) {
        if (csgobj.leaf->geom) primitives.push_back(createCSGPrimitive(csgobj, OpenCSG::Subtraction, highlight_mode, background_mode, OpenSCADOperator::DIFFERENCE));
      }
      if (primitives.size() > 1) {
        OpenCSG::render(primitives);
        GL_CHECKD(glDepthFunc(GL_EQUAL));
      }

      if (shaderinfo && shaderinfo->progid) {
        if (shaderinfo->type != EDGE_RENDERING || (shaderinfo->type == EDGE_RENDERING && showedges)) {
          GL_CHECKD(glUseProgram(shaderinfo->progid));
        }
      }

      for (const auto& csgobj : product.intersections) {
        const auto *ps = dynamic_cast<const PolySet *>(csgobj.leaf->geom.get());
        if (!ps) continue;

        if (shaderinfo && shaderinfo->type == Renderer::SELECT_RENDERING) {
          int identifier = csgobj.leaf->index;
          GL_CHECKD(glUniform3f(shaderinfo->data.select_rendering.identifier,
                                ((identifier >> 0) & 0xff) / 255.0f, ((identifier >> 8) & 0xff) / 255.0f,
                                ((identifier >> 16) & 0xff) / 255.0f));
        }

        const Color4f& c = csgobj.leaf->color;
        csgmode_e csgmode = get_csgmode(highlight_mode, background_mode);

        ColorMode colormode = ColorMode::NONE;
        if (highlight_mode) {
          colormode = ColorMode::HIGHLIGHT;
        } else if (background_mode) {
          colormode = ColorMode::BACKGROUND;
        } else {
          colormode = ColorMode::MATERIAL;
        }

        glPushMatrix();
        glMultMatrixd(csgobj.leaf->matrix.data());

        const Color4f color = setColor(colormode, c.data(), shaderinfo);
        if (color[3] == 1.0f) {
          // object is opaque, draw normally
          render_surface(*ps, csgmode, csgobj.leaf->matrix, shaderinfo);
        } else {
          // object is transparent, so draw rear faces first.  Issue #1496
          glEnable(GL_CULL_FACE);
          glCullFace(GL_FRONT);
          render_surface(*ps, csgmode, csgobj.leaf->matrix, shaderinfo);
          glCullFace(GL_BACK);
          render_surface(*ps, csgmode, csgobj.leaf->matrix, shaderinfo);
          glDisable(GL_CULL_FACE);
        }

        glPopMatrix();
      }
      for (const auto& csgobj : product.subtractions) {
        const auto *ps = dynamic_cast<const PolySet *>(csgobj.leaf->geom.get());
        if (!ps) continue;

        const Color4f& c = csgobj.leaf->color;
        csgmode_e csgmode = get_csgmode(highlight_mode, background_mode, OpenSCADOperator::DIFFERENCE);

        ColorMode colormode = ColorMode::NONE;
        if (highlight_mode) {
          colormode = ColorMode::HIGHLIGHT;
        } else if (background_mode) {
          colormode = ColorMode::BACKGROUND;
        } else {
          colormode = ColorMode::CUTOUT;
        }

        (void) setColor(colormode, c.data(), shaderinfo);
        glPushMatrix();
        glMultMatrixd(csgobj.leaf->matrix.data());
        // negative objects should only render rear faces
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        render_surface(*ps, csgmode, csgobj.leaf->matrix, shaderinfo);
        glDisable(GL_CULL_FACE);

        glPopMatrix();
      }

      if (shaderinfo) glUseProgram(0);
      for (auto& p : primitives) delete p;
      glDepthFunc(GL_LEQUAL);
    }
  } else {
    for (const auto& product : vbo_vertex_products) {
      if (product->primitives().size() > 1) {
        GL_CHECKD(OpenCSG::render(product->primitives()));
        GL_TRACE0("glDepthFunc(GL_EQUAL)");
        GL_CHECKD(glDepthFunc(GL_EQUAL));
      }

      if (shaderinfo && shaderinfo->progid) {
        GL_TRACE("glUseProgram(%d)", shaderinfo->progid);
        GL_CHECKD(glUseProgram(shaderinfo->progid));

        if (shaderinfo->type == EDGE_RENDERING && showedges) {
          shader_attribs_enable();
        }
      }

      for (const auto& vs : product->states()) {
        if (vs) {
          std::shared_ptr<OpenCSGVertexState> csg_vs = std::dynamic_pointer_cast<OpenCSGVertexState>(vs);
          if (csg_vs) {
            if (shaderinfo && shaderinfo->type == SELECT_RENDERING) {
              GL_TRACE("glUniform3f(%d, %f, %f, %f)", shaderinfo->data.select_rendering.identifier %
                       (((csg_vs->csgObjectIndex() >> 0) & 0xff) / 255.0f) %
                       (((csg_vs->csgObjectIndex() >> 8) & 0xff) / 255.0f) %
                       (((csg_vs->csgObjectIndex() >> 16) & 0xff) / 255.0f));
              GL_CHECKD(glUniform3f(shaderinfo->data.select_rendering.identifier,
                                    ((csg_vs->csgObjectIndex() >> 0) & 0xff) / 255.0f,
                                    ((csg_vs->csgObjectIndex() >> 8) & 0xff) / 255.0f,
                                    ((csg_vs->csgObjectIndex() >> 16) & 0xff) / 255.0f));
            }
          }
          std::shared_ptr<VBOShaderVertexState> shader_vs = std::dynamic_pointer_cast<VBOShaderVertexState>(vs);
          if (!shader_vs || (showedges && shader_vs)) {
            vs->draw();
          }
        }
      }

      if (shaderinfo && shaderinfo->progid) {
        GL_TRACE0("glUseProgram(0)");
        GL_CHECKD(glUseProgram(0));

        if (shaderinfo->type == EDGE_RENDERING && showedges) {
          shader_attribs_disable();
        }
      }
      GL_TRACE0("glDepthFunc(GL_LEQUAL)");
      GL_CHECKD(glDepthFunc(GL_LEQUAL));
    }
  }

#endif // ENABLE_OPENCSG
}

BoundingBox OpenCSGRenderer::getBoundingBox() const
{
  BoundingBox bbox;
  if (this->root_products) bbox = this->root_products->getBoundingBox();
  if (this->highlights_products) bbox.extend(this->highlights_products->getBoundingBox());
  if (this->background_products) bbox.extend(this->background_products->getBoundingBox());

  return bbox;
}

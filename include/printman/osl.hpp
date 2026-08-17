#pragma once
//
// OSL edge module: a user-authored OSL shader driving printman's Shader interface. Compiled ONLY
// when PRINTMAN_OSL is set (OSL is a fenced heavy dep). A compiled .oso is loaded, its output
// symbols resolved once, and evaluated per shading point through OSL's ShadingSystem (LLVM-JIT).
//
// OslProgram is a close port of the fork's thread-safe OslDisplaceShader (spikes/osl, oracle-
// matched, TSan-clean). OslShader adapts it to printman: Disp drives geometry, Cout is the "Cout"
// AOV, and the displacement value is also exposed as a "disp" AOV.
//
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <OSL/oslexec.h>
#include <OSL/rendererservices.h>
#include <OSL/shaderglobals.h>
#include <OpenImageIO/errorhandler.h>
#include <OpenImageIO/texture.h>
#include <OpenImageIO/ustring.h>

#include "printman/shader.hpp"

namespace printman {

class OslProgram {
    struct Renderer final : public OSL::RendererServices {
        explicit Renderer(OIIO::TextureSystem* ts) : OSL::RendererServices(ts) {}
        int supports(OSL::string_view) const override { return 0; }
    };
    struct Ctx { OSL::PerThreadInfo* info = nullptr; OSL::ShadingContext* ctx = nullptr; };

public:
    OslProgram(const std::string& searchpath, const std::string& shadername,
               const std::string& disp_output = "Disp", const std::string& color_output = "Cout")
        : m_ts(OIIO::TextureSystem::create(false)), m_rend(m_ts.get()),
          m_output(disp_output), m_color_output(color_output), m_layer("layer1") {
        m_ts->attribute("searchpath", searchpath);
        m_ss = new OSL::ShadingSystem(&m_rend, m_ts.get(), &m_err);
        OSL::ustring sp(searchpath);
        m_ss->attribute("searchpath:shader", OIIO::TypeDesc::STRING, &sp);
        m_group = m_ss->ShaderGroupBegin("printman");
        if (!m_ss->Shader(*m_group, "surface", shadername, m_layer)) {
            m_group.reset(); delete m_ss; m_ss = nullptr;
            throw std::runtime_error("OSL Shader() failed for '" + shadername + "'");
        }
        m_ss->ShaderGroupEnd(*m_group);
        OSL::ustring outs[] = {m_output, m_color_output};
        m_ss->attribute(m_group.get(), "renderer_outputs", OIIO::TypeDesc(OIIO::TypeDesc::STRING, 2), &outs[0]);
        OSL::PerThreadInfo* ti = m_ss->create_thread_info();
        OSL::ShadingContext* c = m_ss->get_context(ti);
        OSL::ShaderGlobals sg; std::memset((void*)&sg, 0, sizeof(sg)); sg.renderer = &m_rend;
        m_ss->execute(*c, *m_group, sg, false);
        m_sym = m_ss->find_symbol(*m_group, m_layer, m_output);
        if (m_sym && m_ss->symbol_typedesc(m_sym).basetype != OIIO::TypeDesc::FLOAT) m_sym = nullptr;
        m_color_sym = m_ss->find_symbol(*m_group, m_layer, m_color_output);
        if (m_color_sym) {
            const OIIO::TypeDesc td = m_ss->symbol_typedesc(m_color_sym);
            if (!(td.basetype == OIIO::TypeDesc::FLOAT && td.basevalues() == 3)) m_color_sym = nullptr;
        }
        m_ss->release_context(c); m_ss->destroy_thread_info(ti);
        if (!m_sym && !m_color_sym) {
            m_group.reset(); delete m_ss; m_ss = nullptr;
            throw std::runtime_error("OSL shader '" + shadername + "' has neither Disp nor Cout");
        }
    }
    OslProgram(const OslProgram&) = delete;
    OslProgram& operator=(const OslProgram&) = delete;
    ~OslProgram() {
        if (m_ss) {
            for (auto& kv : m_ctxs) { m_ss->release_context(kv.second.ctx); m_ss->destroy_thread_info(kv.second.info); }
            m_group.reset(); delete m_ss;
        }
    }
    bool has_disp() const { return m_sym != nullptr; }
    bool has_color() const { return m_color_sym != nullptr; }

    double displace(const V3& p, const V3& n, double u, double v) const {
        if (!m_sym) return 0.0;
        OSL::ShadingContext* ctx = run(p, n, u, v);
        const void* a = m_ss->symbol_address(*ctx, m_sym);
        return a ? double(*reinterpret_cast<const float*>(a)) : 0.0;
    }
    V3 color(const V3& p, const V3& n, double u, double v) const {
        if (!m_color_sym) return V3{{0, 0, 0}};
        OSL::ShadingContext* ctx = run(p, n, u, v);
        const void* a = m_ss->symbol_address(*ctx, m_color_sym);
        if (!a) return V3{{0, 0, 0}};
        const float* c = reinterpret_cast<const float*>(a);
        return V3{{double(c[0]), double(c[1]), double(c[2])}};
    }

private:
    OSL::ShadingContext* run(const V3& p, const V3& n, double u, double v) const {
        OSL::ShadingContext* ctx = ctx_for_thread();
        OSL::ShaderGlobals sg; std::memset((void*)&sg, 0, sizeof(sg));
        sg.P = OSL::Vec3((float)p[0], (float)p[1], (float)p[2]);
        sg.N = OSL::Vec3((float)n[0], (float)n[1], (float)n[2]); sg.Ng = sg.N;
        sg.u = (float)u; sg.v = (float)v;
        sg.renderer = const_cast<Renderer*>(&m_rend);
        m_ss->execute(*ctx, *m_group, sg);
        return ctx;
    }
    OSL::ShadingContext* ctx_for_thread() const {
        const std::thread::id id = std::this_thread::get_id();
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_ctxs.find(id);
        if (it == m_ctxs.end()) { Ctx c; c.info = m_ss->create_thread_info(); c.ctx = m_ss->get_context(c.info); it = m_ctxs.emplace(id, c).first; }
        return it->second.ctx;
    }
    std::shared_ptr<OIIO::TextureSystem> m_ts;
    Renderer m_rend;
    OIIO::ErrorHandler m_err;
    OSL::ShadingSystem* m_ss = nullptr;
    OSL::ShaderGroupRef m_group;
    const OSL::ShaderSymbol* m_sym = nullptr;
    const OSL::ShaderSymbol* m_color_sym = nullptr;
    OSL::ustring m_output, m_color_output, m_layer;
    mutable std::mutex m_mtx;
    mutable std::unordered_map<std::thread::id, Ctx> m_ctxs;
};

// Adapt one or two OSL programs (a displacement shader and/or a colour shader) to printman::Shader.
class OslShader : public Shader {
    std::unique_ptr<OslProgram> disp_, col_;
    double reach_;
    std::vector<std::string> aovs_;

public:
    OslShader(const std::string& dir, const std::string& disp_name, const std::string& color_name, double reach)
        : reach_(reach) {
        if (!disp_name.empty()) disp_ = std::make_unique<OslProgram>(dir, disp_name);
        if (!color_name.empty()) col_ = std::make_unique<OslProgram>(dir, color_name);
        if (col_ && col_->has_color()) aovs_.push_back("Cout");
        aovs_.push_back("disp");
    }
    double displace(const V3& p, const V3& n, const V2& uv) const override {
        return disp_ ? disp_->displace(p, n, uv[0], uv[1]) : 0.0;
    }
    std::vector<std::string> aov_names() const override { return aovs_; }
    void shade(const V3& p, const V3& n, const V2& uv, V3* out) const override {
        size_t k = 0;
        if (col_ && col_->has_color()) out[k++] = col_->color(p, n, uv[0], uv[1]);
        double d = disp_ ? disp_->displace(p, n, uv[0], uv[1]) : 0.0;
        double dn = reach_ > 0 ? d / reach_ : 0.0;
        out[k] = V3{{dn, dn, dn}};
    }
    double max_reach() const override { return reach_; }
};

}  // namespace printman

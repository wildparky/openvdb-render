#include "vdb_subscene_override.h"

#include "vdb_maya_utils.hpp"

#include <maya/MGlobal.h>
#include <maya/MFnDagNode.h>
#include <maya/MDrawContext.h>

#include <tbb/task_scheduler_init.h>
#include <tbb/parallel_for.h>

#include <new>
#include <random>

namespace {
    // We have to options to code shaders, either cgfx, which is deprecated since 2012
    // or ogsfx, which is a glslfx like thing and severely underdocumented.
    // I decided to go with ogsfx, that can be reused easier later on in other
    // packages like katana. -- Pal
    // Best example for ogsfx https://knowledge.autodesk.com/search-result/caas/CloudHelp/cloudhelp/2016/ENU/Maya-SDK/files/GUID-94505429-12F9-4F04-A4D9-B80880AD0BA1-htm.html

    // Fun part comes, when maya is not giving you error messages when the shader is invalid.
    // Awesome, right?

    // For random numbers : http://www.reedbeta.com/blog/quick-and-easy-gpu-random-numbers-in-d3d11/
    const char* point_cloud_technique = R"ogsfx(
uniform mat4 wv_mat : WorldView;
uniform mat4 p_mat : Projection;
uniform float point_size;
uniform float voxel_size;
uniform float half_viewport_size;
uniform vec3 jitter_size;
uniform int vertex_count;

attribute vs_input
{
    vec3 in_position : POSITION;
    vec4 in_color : TEXCOORD0;
};

attribute vs_to_ps
{
    vec4 point_color;
};

attribute ps_output
{
    vec4 out_color : COLOR0;
}

GLSLShader VS
{
    float rand_xorshift(uint seed)
    {
        seed ^= (seed << 13);
        seed ^= (seed << 17);
        seed ^= (seed << 5);
        return float(seed) * (1.0 / 4294967296.0);
    }

    void main()
    {
        vec4 pos = vec4(in_position, 1.0);
        pos.x += jitter_size.x * 2.0 * rand_xorshift(uint(gl_VertexID)) - jitter_size.x;
        pos.y += jitter_size.y * 2.0 * rand_xorshift(uint(gl_VertexID + vertex_count)) - jitter_size.y;
        pos.z += jitter_size.z * 2.0 * rand_xorshift(uint(gl_VertexID + vertex_count * 2)) - jitter_size.z;
        pos = wv_mat * pos;
        vec4 proj_pos = p_mat * vec4(pos.x + point_size * voxel_size, pos.y, pos.z, pos.w);
        gl_Position = p_mat * pos;
        gl_PointSize = abs(proj_pos.x / proj_pos.w - gl_Position.x / gl_Position.w) * half_viewport_size;
        vsOut.point_color = in_color;
    }
}

GLSLShader PS
{
    void main()
    {
        out_color = psIn.point_color;
    }
}

technique Main
{
    pass p0
    {
        VertexShader(in vs_input, out vs_to_ps vsOut) = VS;
        PixelShader(in vs_to_ps psIn, out ps_output) = PS;
    }
}
    )ogsfx";

    const MHWRender::MShaderManager* get_shader_manager()
    {
        auto renderer = MHWRender::MRenderer::theRenderer();
        if (renderer == nullptr) {
            return nullptr;
        }

        auto shader_manager = renderer->getShaderManager();
        if (shader_manager == nullptr) {
            return nullptr;
        }

        return shader_manager;
    }

    // This is a hacky workaround for having a callback specific dataset
    // We expect maya not to run other draw calls between the pre and post renders
    bool point_size_enabled = false;

    void pre_point_cloud_render(MHWRender::MDrawContext& context,
                                const MHWRender::MRenderItemList& /*renderItemList*/,
                                MHWRender::MShaderInstance* shaderInstance)
    {
        int origin_x = 0;
        int origin_y = 0;
        int width = 0;
        int height = 0;
        context.getViewportDimensions(origin_x, origin_y, width, height);
        shaderInstance->setParameter("half_viewport_size", static_cast<float>(width) * 0.5f);
        point_size_enabled = glIsEnabled(GL_VERTEX_PROGRAM_POINT_SIZE) != 0;
        if (!point_size_enabled) {
            glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
        }
    }

    void post_point_cloud_render(MHWRender::MDrawContext& /*context*/,
                                 const MHWRender::MRenderItemList& /*renderItemList*/,
                                 MHWRender::MShaderInstance* /*shaderInstance*/)
    {
        if (!point_size_enabled) {
            glDisable(GL_VERTEX_PROGRAM_POINT_SIZE);
        }
    }
}

namespace MHWRender {
    void VDBSubSceneOverride::shader_instance_deleter::operator()(MShaderInstance* p)
    {
        auto shmgr = get_shader_manager();
        if (shmgr != nullptr) {
            shmgr->releaseShader(p);
        }
    }

    struct VDBSubSceneOverrideData {
        MBoundingBox bbox;

        MMatrix world_matrix;

        MFloatVector scattering_color;
        MFloatVector attenuation_color;
        MFloatVector emission_color;

        std::string attenuation_channel;
        std::string scattering_channel;
        std::string emission_channel;

        Gradient scattering_gradient;
        Gradient attenuation_gradient;
        Gradient emission_gradient;

        std::unique_ptr<openvdb::io::File> vdb_file;
        openvdb::GridBase::ConstPtr scattering_grid;
        openvdb::GridBase::ConstPtr attenuation_grid;
        openvdb::GridBase::ConstPtr emission_grid;

        openvdb::Vec3f voxel_size;

        float point_size;
        float point_jitter;

        int vertex_count;
        int point_skip;
        int update_trigger;
        VDBDisplayMode display_mode;
        VDBShaderMode shader_mode;

        bool data_has_changed;
        bool shader_has_changed;
        bool visible;
        bool old_bounding_box_enabled;
        bool old_point_cloud_enabled;

        VDBSubSceneOverrideData() :
            voxel_size(std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()),
            point_size(std::numeric_limits<float>::infinity()), point_jitter(std::numeric_limits<float>::infinity()),
            vertex_count(0), point_skip(-1), update_trigger(-1),
            display_mode(DISPLAY_AXIS_ALIGNED_BBOX), shader_mode(SHADER_MODE_VOLUME_COLLECTOR),
            data_has_changed(false), shader_has_changed(false), visible(true),
            old_bounding_box_enabled(true), old_point_cloud_enabled(true)
        {
            for (unsigned int x = 0; x < 4; ++x) {
                for (unsigned int y = 0; y < 4; ++y) {
                    world_matrix(x, y) = std::numeric_limits<float>::infinity();
                }
            }
        }

        ~VDBSubSceneOverrideData()
        {
            clear();
        }

        void clear()
        {
            scattering_grid = 0;
            attenuation_grid = 0;
            emission_grid = 0;
            vdb_file.reset();
        }

        bool update(const VDBVisualizerData* data, const MObject& obj)
        {
            MDagPath dg = MDagPath::getAPathTo(obj);
            const MMatrix inc_world_matrix = dg.inclusiveMatrix();
            const bool matrix_changed = world_matrix != inc_world_matrix;
            if (matrix_changed) {
                world_matrix = inc_world_matrix;
            }
            // TODO: we can limit some of the comparisons to the display mode
            // ie, we don't need to compare certain things if we are using the bounding
            // box mode

            auto path_is_visible = [&dg]() -> bool {
                MDagPath dg_copy = dg;
                MFnDagNode dg_node;
                for (MStatus status = MS::kSuccess; status; status = dg_copy.pop()) {
                    dg_node.setObject(dg_copy.node());
                    if (dg_node.isIntermediateObject()) {
                        return false;
                    } else if (!dg_node.findPlug("visibility").asBool()) {
                        return false;
                    }
                }
                return true;
            };

            const bool visibility_changed = setup_parameter(visible, path_is_visible());

            if (data == nullptr || update_trigger == data->update_trigger) {
                return matrix_changed | visibility_changed;
            }

            update_trigger = data->update_trigger;

            const std::string& filename = data->vdb_path;
            auto open_file = [&]() {
                data_has_changed |= true;
                clear();

                try {
                    vdb_file.reset(new openvdb::io::File(filename));
                }
                catch (...) {
                    vdb_file.reset();
                }
            };

            if (filename.empty()) {
                data_has_changed |= true;
                clear();
            }
            else if (vdb_file == nullptr) {
                open_file();
            }
            else if (filename != vdb_file->filename()) {
                open_file();
            }

            data_has_changed |= setup_parameter(display_mode, data->display_mode);
            data_has_changed |= setup_parameter(shader_mode, data->shader_mode);
            data_has_changed |= setup_parameter(bbox, data->bbox);
            data_has_changed |= setup_parameter(scattering_color, data->scattering_color);
            data_has_changed |= setup_parameter(attenuation_color, data->attenuation_color);
            data_has_changed |= setup_parameter(emission_color, data->emission_color);
            data_has_changed |= setup_parameter(attenuation_channel, data->attenuation_channel);
            data_has_changed |= setup_parameter(scattering_channel, data->scattering_channel);
            data_has_changed |= setup_parameter(emission_channel, data->emission_channel);
            data_has_changed |= setup_parameter(scattering_gradient, data->scattering_gradient);
            data_has_changed |= setup_parameter(attenuation_gradient, data->attenuation_gradient);
            data_has_changed |= setup_parameter(emission_gradient, data->emission_gradient);
            data_has_changed |= setup_parameter(point_skip, data->point_skip);

            shader_has_changed |= setup_parameter(point_size, data->point_size);
            shader_has_changed |= setup_parameter(point_jitter, data->point_jitter);

            return data_has_changed | shader_has_changed | matrix_changed | visibility_changed;
        }
    };

    MString VDBSubSceneOverride::registrantId("VDBVisualizerSubSceneOverride");

    MPxSubSceneOverride* VDBSubSceneOverride::creator(const MObject& obj)
    {
        return new VDBSubSceneOverride(obj);
    }

    VDBSubSceneOverride::VDBSubSceneOverride(const MObject& obj) : MPxSubSceneOverride(obj),
                                                                   p_data(new VDBSubSceneOverrideData)
    {
        m_object = obj;
        MFnDependencyNode dnode(obj);
        p_vdb_visualizer = dynamic_cast<VDBVisualizerShape*>(dnode.userNode());
        auto shmgr = get_shader_manager();
        if (shmgr != nullptr) {
            p_point_cloud_shader.reset(shmgr->getEffectsBufferShader(
                point_cloud_technique, static_cast<unsigned int>(strlen(point_cloud_technique)), "Main", 0, 0, false,
                pre_point_cloud_render, post_point_cloud_render));

            if (p_point_cloud_shader != nullptr) {
                p_point_cloud_shader->setIsTransparent(true);
            }
            else {
                MGlobal::displayError(MString("[vdb_subscene_override] Error compiling point cloud shader!"));
            }
        }
    }

    VDBSubSceneOverride::~VDBSubSceneOverride()
    {
    }

    MHWRender::DrawAPI VDBSubSceneOverride::supportedDrawAPIs() const
    {
#if MAYA_API_VERSION >= 201600
        return kOpenGLCoreProfile | kOpenGL;
#else
        return kOpenGL;
#endif
    }

    void VDBSubSceneOverride::update(MSubSceneContainer& container, const MFrameContext& /*frameContext*/)
    {
        VDBSubSceneOverrideData* data = p_data.get();

        MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
        if (renderer == nullptr) {
            return;
        }

        const MHWRender::MShaderManager* shader_manager = renderer->getShaderManager();
        if (shader_manager == nullptr) {
            return;
        }

        MRenderItem* bounding_box = container.find("bounding_box");
        if (bounding_box == nullptr) {
            bounding_box = MHWRender::MRenderItem::Create("bounding_box",
                                                          MRenderItem::NonMaterialSceneItem,
                                                          MGeometry::kLines);
            bounding_box->enable(false);
            bounding_box->setDrawMode(MGeometry::kAll);
            bounding_box->depthPriority(MRenderItem::sDormantWireDepthPriority);

            MHWRender::MShaderInstance* shader = shader_manager->getStockShader(
                MHWRender::MShaderManager::k3dSolidShader, nullptr, nullptr);
            if (shader) {
                // Set the color on the shader instance using the parameter interface
                static const float color[] = {0.0f, 1.0f, 0.0f, 1.0f};
                shader->setParameter("solidColor", color);

                // Assign the shader to the custom render item
                bounding_box->setShader(shader);
            }

            container.add(bounding_box);
        }

        MHWRender::MRenderItem* point_cloud = container.find("point_cloud");
        if (point_cloud == nullptr) {
            point_cloud = MHWRender::MRenderItem::Create("point_cloud",
                                                         MHWRender::MGeometry::kPoints,
                                                         MHWRender::MGeometry::kAll,
                                                         false);
            point_cloud->enable(false);
            point_cloud->setDrawMode(MGeometry::kAll);
            point_cloud->depthPriority(MRenderItem::sDormantPointDepthPriority);

            if (p_point_cloud_shader == nullptr) {
                MHWRender::MShaderInstance* shader = shader_manager->getStockShader(
                    MHWRender::MShaderManager::k3dCPVFatPointShader, nullptr, nullptr);
                if (shader) {
                    point_cloud->setShader(shader);
                }
            }
            else {
                point_cloud->setShader(p_point_cloud_shader.get());
            }

            container.add(point_cloud);
        }

        if (!data->visible) {
            data->old_point_cloud_enabled = point_cloud->isEnabled();
            data->old_bounding_box_enabled = bounding_box->isEnabled();
            point_cloud->enable(false);
            bounding_box->enable(false);
            return;
        } else {
            point_cloud->enable(data->old_point_cloud_enabled);
            bounding_box->enable(data->old_bounding_box_enabled);
        }

        bounding_box->setMatrix(&data->world_matrix);
        point_cloud->setMatrix(&data->world_matrix);

        if (data->data_has_changed) {
            const bool file_exists = data->vdb_file != nullptr;

            const static MVertexBufferDescriptor position_buffer_desc("", MGeometry::kPosition, MGeometry::kFloat, 3);
            const static MVertexBufferDescriptor color_buffer_desc("", MGeometry::kTexture, MGeometry::kFloat, 4);

            if (!file_exists || data->display_mode <= DISPLAY_GRID_BBOX) {
                point_cloud->enable(false);
                bounding_box->enable(true);
                data->old_point_cloud_enabled = false;
                data->old_bounding_box_enabled = true;

                MVertexBufferArray vertex_buffers;
                p_bbox_position.reset(new MVertexBuffer(position_buffer_desc));
                p_bbox_indices.reset(new MIndexBuffer(MGeometry::kUnsignedInt32));

                if ((data->display_mode == DISPLAY_AXIS_ALIGNED_BBOX) || !file_exists) {
                    MFloatVector* bbox_vertices = reinterpret_cast<MFloatVector*>(p_bbox_position->acquire(8, true));
                    MFloatVector min = data->bbox.min();
                    MFloatVector max = data->bbox.max();
                    bbox_vertices[0] = MFloatVector(min.x, min.y, min.z);
                    bbox_vertices[1] = MFloatVector(min.x, max.y, min.z);
                    bbox_vertices[2] = MFloatVector(min.x, max.y, max.z);
                    bbox_vertices[3] = MFloatVector(min.x, min.y, max.z);
                    bbox_vertices[4] = MFloatVector(max.x, min.y, min.z);
                    bbox_vertices[5] = MFloatVector(max.x, max.y, min.z);
                    bbox_vertices[6] = MFloatVector(max.x, max.y, max.z);
                    bbox_vertices[7] = MFloatVector(max.x, min.y, max.z);
                    p_bbox_position->commit(bbox_vertices);
                    set_bbox_indices(1, p_bbox_indices.get());
                }
                else if (data->display_mode == DISPLAY_GRID_BBOX) {
                    try {
                        if (!data->vdb_file->isOpen()) {
                            data->vdb_file->open(false);
                        }
                        openvdb::GridPtrVecPtr grids = data->vdb_file->readAllGridMetadata();
                        if (grids->size() == 0) {
                            return;
                        }
                        std::vector<MFloatVector> vertices;
                        vertices.reserve(grids->size() * 8);

                        for (openvdb::GridPtrVec::const_iterator it = grids->begin(); it != grids->end(); ++it) {
                            if (openvdb::GridBase::ConstPtr grid = *it) {
                                std::array<MFloatVector, 8> _vertices;
                                if (read_grid_transformed_bbox_wire(grid, _vertices)) {
                                    for (int v = 0; v < 8; ++v) {
                                        vertices.push_back(_vertices[v]);
                                    }
                                }
                            }
                        }

                        const unsigned int vertex_count = static_cast<unsigned int>(vertices.size());

                        if (vertex_count > 0) {
                            p_bbox_position.reset(new MVertexBuffer(position_buffer_desc));
                            p_bbox_indices.reset(new MIndexBuffer(MGeometry::kUnsignedInt32));
                            MFloatVector* bbox_vertices = reinterpret_cast<MFloatVector*>(p_bbox_position->acquire(
                                vertex_count, true));
                            for (unsigned int i = 0; i < vertex_count; ++i) {
                                bbox_vertices[i] = vertices[i];
                            }
                            p_bbox_position->commit(bbox_vertices);
                            set_bbox_indices(vertex_count / 8, p_bbox_indices.get());
                        }
                    }
                    catch (...) {
                    }
                }

                vertex_buffers.addBuffer("", p_bbox_position.get());
                setGeometryForRenderItem(*bounding_box, vertex_buffers, *p_bbox_indices.get(), &data->bbox);
            }
            else {
                data->old_bounding_box_enabled = false;
                bounding_box->enable(false);
                if (data->display_mode == DISPLAY_POINT_CLOUD) {
                    try {
                        if (!data->vdb_file->isOpen()) {
                            data->vdb_file->open(false);
                        }
                        if (data->attenuation_grid == nullptr ||
                            data->attenuation_grid->getName() != data->attenuation_channel) {
                            data->attenuation_grid = data->vdb_file->readGrid(data->attenuation_channel);
                        }
                    }
                    catch (...) {
                        data->attenuation_grid = nullptr;
                        data->scattering_grid = nullptr;
                        data->emission_grid = nullptr;
                        return;
                    }

                    data->old_point_cloud_enabled = true;
                    point_cloud->enable(true);

                    data->voxel_size = data->attenuation_grid->voxelSize();

                    FloatVoxelIterator* iter = nullptr;

                    if (data->attenuation_grid->valueType() == "float") {
                        iter = new FloatToFloatVoxelIterator(
                            openvdb::gridConstPtrCast<openvdb::FloatGrid>(data->attenuation_grid));
                    }
                    else if (data->attenuation_grid->valueType() == "vec3s") {
                        iter = new Vec3SToFloatVoxelIterator(
                            openvdb::gridConstPtrCast<openvdb::Vec3SGrid>(data->attenuation_grid));
                    }
                    else {
                        iter = new FloatVoxelIterator();
                    }

                    // setting up vertex buffers
                    std::vector<MFloatVector> vertices;
                    vertices.reserve(iter->get_active_voxels());
                    const openvdb::math::Transform attenuation_transform = data->attenuation_grid->transform();

                    std::mt19937 mt_generator;
                    std::uniform_real_distribution<float> uniform_0_1_dist(0.0f, 1.0f);
                    const float point_skip_ratio = 1.0f / static_cast<float>(std::max(data->point_skip, 1));
                    for (; iter->is_valid(); iter->get_next()) {
                        // this gives a better distribution than skipping based on index
                        if (uniform_0_1_dist(mt_generator) > point_skip_ratio) {
                            continue;
                        }
                        openvdb::Vec3f vdb_pos = attenuation_transform.indexToWorld(iter->get_coord());
                        vertices.push_back(
                            MFloatVector(vdb_pos.x(), vdb_pos.y(),
                                         vdb_pos.z()));
                    }

                    vertices.shrink_to_fit();
                    const unsigned int vertex_count = static_cast<unsigned int>(vertices.size());

                    if (vertex_count == 0) {
                        return;
                    }

                    data->vertex_count = static_cast<int>(vertex_count);

                    delete iter;

                    tbb::task_scheduler_init task_init;

                    p_index_buffer.reset(new MIndexBuffer(MGeometry::kUnsignedInt32));
                    unsigned int* indices = reinterpret_cast<unsigned int*>(p_index_buffer->acquire(vertex_count,
                                                                                                    true));
                    for (unsigned int i = 0; i < vertex_count; ++i) {
                        indices[i] = i;
                    }
                    p_index_buffer->commit(indices);


                    p_position_buffer.reset(new MVertexBuffer(position_buffer_desc));
                    MFloatVector* pc_vertices = reinterpret_cast<MFloatVector*>(p_position_buffer->acquire(
                        static_cast<unsigned int>(vertices.size()), true));

                    tbb::parallel_for(tbb::blocked_range<unsigned int>(0, vertex_count),
                                      [&](const tbb::blocked_range<unsigned int>& r) {
                                          memcpy(&pc_vertices[r.begin()], &vertices[r.begin()],
                                                 (r.end() - r.begin()) * sizeof(MFloatVector));
                                      });
                    p_position_buffer->commit(pc_vertices);

                    // setting up color buffers

                    try {
                        if (data->scattering_grid == nullptr ||
                            data->scattering_grid->getName() != data->scattering_channel) {
                            data->scattering_grid = data->vdb_file->readGrid(data->scattering_channel);
                        }
                    }
                    catch (...) {
                        data->scattering_grid = nullptr;
                    }

                    RGBSampler* scattering_sampler = nullptr;

                    if (data->scattering_grid == nullptr) {
                        scattering_sampler = new(m_scattering_sampler.data()) RGBSampler();
                    }
                    else {
                        if (data->scattering_grid->valueType() == "float") {
                            scattering_sampler = new(m_scattering_sampler.data()) FloatToRGBSampler(
                                openvdb::gridConstPtrCast<openvdb::FloatGrid>(data->scattering_grid));
                        }
                        else if (data->scattering_grid->valueType() == "vec3s") {
                            scattering_sampler = new(m_scattering_sampler.data()) Vec3SToRGBSampler(
                                openvdb::gridConstPtrCast<openvdb::Vec3SGrid>(data->scattering_grid));
                        }
                        else {
                            scattering_sampler = new(m_scattering_sampler.data()) RGBSampler();
                        }
                    }

                    try {
                        if (data->emission_grid == nullptr ||
                            data->emission_grid->getName() != data->emission_channel) {
                            data->emission_grid = data->vdb_file->readGrid(data->emission_channel);
                        }
                    }
                    catch (...) {
                        data->emission_grid = nullptr;
                    }

                    RGBSampler* emission_sampler = nullptr;

                    if (data->emission_grid == nullptr) {
                        emission_sampler = new(m_emission_sampler.data()) RGBSampler(MFloatVector(0.0f, 0.0f, 0.0f));
                    }
                    else {
                        if (data->emission_grid->valueType() == "float") {
                            emission_sampler = new(m_emission_sampler.data()) FloatToRGBSampler(
                                openvdb::gridConstPtrCast<openvdb::FloatGrid>(data->emission_grid));
                        }
                        else if (data->emission_grid->valueType() == "vec3s") {
                            emission_sampler = new(m_emission_sampler.data()) Vec3SToRGBSampler(
                                openvdb::gridConstPtrCast<openvdb::Vec3SGrid>(data->emission_grid));
                        }
                        else {
                            emission_sampler = new(m_emission_sampler.data()) RGBSampler(
                                MFloatVector(0.0f, 0.0f, 0.0f));
                        }
                    }

                    RGBSampler* attenuation_sampler = 0;

                    if (data->attenuation_grid->valueType() == "float") {
                        attenuation_sampler = new(m_attenuation_sampler.data()) FloatToRGBSampler(
                            openvdb::gridConstPtrCast<openvdb::FloatGrid>(data->attenuation_grid));
                    }
                    else if (data->attenuation_grid->valueType() == "vec3s") {
                        attenuation_sampler = new(m_attenuation_sampler.data()) Vec3SToRGBSampler(
                            openvdb::gridConstPtrCast<openvdb::Vec3SGrid>(data->attenuation_grid));
                    }
                    else {
                        attenuation_sampler = new(m_attenuation_sampler.data()) RGBSampler(
                            MFloatVector(1.0f, 1.0f, 1.0f));
                    }

                    p_color_buffer.reset(new MVertexBuffer(color_buffer_desc));
                    MColor* colors = reinterpret_cast<MColor*>(p_color_buffer->acquire(vertex_count, true));
                    tbb::parallel_for(tbb::blocked_range<unsigned int>(0, vertex_count),
                                      [&](const tbb::blocked_range<unsigned int>& r) {
                                          for (unsigned int i = r.begin(); i < r.end(); ++i) {
                                              const MFloatVector& v = vertices[i];
                                              const openvdb::Vec3d pos(v.x, v.y, v.z);
                                              MColor& color = colors[i];
                                              const MFloatVector scattering_color = data->scattering_gradient.evaluate(
                                                  scattering_sampler->get_rgb(pos));
                                              const MFloatVector emission_color = data->emission_gradient.evaluate(
                                                  emission_sampler->get_rgb(pos));
                                              const MFloatVector attenuation_color = data->attenuation_gradient.evaluate(
                                                  attenuation_sampler->get_rgb(pos));
                                              color.r = scattering_color.x * data->scattering_color.x +
                                                        emission_color.x * data->emission_color.x;
                                              color.g = scattering_color.y * data->scattering_color.y +
                                                        emission_color.y * data->emission_color.y;
                                              color.b = scattering_color.z * data->scattering_color.z +
                                                        emission_color.z * data->emission_color.z;
                                              color.a = (attenuation_color.x * data->attenuation_color.x +
                                                         attenuation_color.y * data->attenuation_color.y +
                                                         attenuation_color.z * data->attenuation_color.z) / 3.0f;
                                          }
                                      });
                    p_color_buffer->commit(colors);

                    scattering_sampler->~RGBSampler();
                    emission_sampler->~RGBSampler();
                    attenuation_sampler->~RGBSampler();

                    MVertexBufferArray vertex_buffers;
                    vertex_buffers.addBuffer("", p_position_buffer.get());
                    vertex_buffers.addBuffer("", p_color_buffer.get());
                    setGeometryForRenderItem(*point_cloud, vertex_buffers, *p_index_buffer.get(), &data->bbox);
                }
            }
            p_point_cloud_shader->setParameter("vertex_count", data->vertex_count);
            p_point_cloud_shader->setParameter("voxel_size",
                                               std::max(data->voxel_size.x(),
                                                        std::max(data->voxel_size.y(), data->voxel_size.z())));
            data->data_has_changed = false;
        }

        // Setting up shader parameters
        if (data->shader_has_changed && p_point_cloud_shader) {
            p_point_cloud_shader->setParameter("point_size", data->point_size);
            p_point_cloud_shader->setParameter("jitter_size", MFloatVector(
                data->voxel_size.x(), data->voxel_size.y(), data->voxel_size.y()) * data->point_jitter);
            data->shader_has_changed = false;
        }
    }

    bool VDBSubSceneOverride::requiresUpdate(const MSubSceneContainer& /*container*/,
                                             const MFrameContext& /*frameContext*/) const
    {

        return p_data->update(p_vdb_visualizer->get_update(), m_object);
    }
}

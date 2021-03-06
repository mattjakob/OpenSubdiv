//
//     Copyright 2013 Pixar
//
//     Licensed under the Apache License, Version 2.0 (the "License");
//     you may not use this file except in compliance with the License
//     and the following modification to it: Section 6 Trademarks.
//     deleted and replaced with:
//
//     6. Trademarks. This License does not grant permission to use the
//     trade names, trademarks, service marks, or product names of the
//     Licensor and its affiliates, except as required for reproducing
//     the content of the NOTICE file.
//
//     You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
//     Unless required by applicable law or agreed to in writing,
//     software distributed under the License is distributed on an
//     "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//     either express or implied.  See the License for the specific
//     language governing permissions and limitations under the
//     License.
//

// ### OpenSubdivShaderOverride.cpp
//
//      Viewport 2.0 override for OpenSubdivShader, implementing
//      custom shading for OpenSubdiv patches.

#if defined(__APPLE__)
    #include <maya/OpenMayaMac.h>
#else
    #include <GL/glew.h>
    #if defined(WIN32)
        #include <GL/wglew.h>
    #endif
#endif


// Include this first to avoid winsock2.h problems on Windows:
#include <maya/MTypes.h>

#include <maya/MFnPlugin.h>
#include <maya/MFnPluginData.h>
#include <maya/MFnMesh.h>
#include <maya/MGlobal.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MIntArray.h>
#include <maya/MUintArray.h>
#include <maya/MPointArray.h>
#include <maya/MNodeMessage.h>

#include <maya/MShaderManager.h>
#include <maya/MViewport2Renderer.h>
#include <maya/MDrawRegistry.h>
#include <maya/MDrawContext.h>
#include <maya/MHWShaderSwatchGenerator.h>
#include <maya/MPxVertexBufferGenerator.h>
#include <maya/MStateManager.h>

#include "../common/maya_util.h"                // for CHECK_GL_ERROR
#include "OpenSubdivShaderOverride.h"
#include "OpenSubdivShader.h"
#include "osdMeshData.h"

using MHWRender::MVertexBuffer;
using MHWRender::MVertexBufferDescriptor;
using MHWRender::MDepthStencilState;
using MHWRender::MDepthStencilStateDesc;
using MHWRender::MBlendState;
using MHWRender::MBlendStateDesc;
using MHWRender::MComponentDataIndexing;
#if MAYA_API_VERSION >= 201350
using MHWRender::MVertexBufferArray;
#endif


// Set up compute controllers for each available compute kernel.
#include <osd/cpuComputeController.h>
OpenSubdiv::OsdCpuComputeController *g_cpuComputeController = 0;

#ifdef OPENSUBDIV_HAS_OPENMP
    #include <osd/ompComputeController.h>

    OpenSubdiv::OsdOmpComputeController *g_ompComputeController = 0;
#endif

#ifdef OPENSUBDIV_HAS_OPENCL
    #include <osd/clComputeController.h>
    cl_context g_clContext;
    cl_command_queue g_clQueue;

    #include "../common/clInit.h"

    OpenSubdiv::OsdCLComputeController *g_clComputeController = 0;
#endif

#ifdef OPENSUBDIV_HAS_CUDA
    #include <osd/cudaComputeController.h>

    extern void cudaInit();
    OpenSubdiv::OsdCudaComputeController *g_cudaComputeController = 0;
#endif

OpenSubdivShaderOverride::OpenSubdivShaderOverride(const MObject &obj)
    : MHWRender::MPxShaderOverride(obj),
      _shader(NULL)
{
}

OpenSubdivShaderOverride::~OpenSubdivShaderOverride()
{
    MMessage::removeCallbacks(_callbackIds);
}

MHWRender::MPxShaderOverride*
OpenSubdivShaderOverride::creator(const MObject &obj)
{
    return new OpenSubdivShaderOverride(obj);
}


//
// #### attrChangedCB
//
//      Informs us whenever an attribute on the shape node changes.
//      Overkill since we really only want to know if the topology 
//      changes (e.g. an edge crease is added or changed) but Maya 
//      doesn't give us access to a callback that fine-grained. 
//      MMessage::PolyTopologyChangedCallback sounds promising 
//      but only calls back on a single change for any edit 
//      (i.e. not while dragging).
//
/*static*/
void 
OpenSubdivShaderOverride::attrChangedCB(MNodeMessage::AttributeMessage msg, MPlug & plug,
                                        MPlug & otherPlug, void* clientData)
{
    // We only care if the plug is outMesh and the action is "evaluate"
    if ( msg & MNodeMessage::kAttributeEval ) {
        OsdMeshData *meshData = (OsdMeshData*)clientData;
        MFnDependencyNode depNodeFn(meshData->getDagPath().node());
        if ( plug == depNodeFn.attribute("outMesh")) {
            meshData->setMeshTopoDirty();
        }
    }
}


//
// #### addTopologyChangedCallbacks
//
//      Add a callback to inform us when topology might be changing 
//      so we can update the HBR mesh accordingly.
//
void
OpenSubdivShaderOverride::addTopologyChangedCallbacks( const MDagPath& dagPath, OsdMeshData *data )
{
    MStatus status = MS::kSuccess;

    // Extract shape node and add callback to let us know when an attribute changes
    MDagPath meshDagPath = dagPath;
    meshDagPath.extendToShape();
    MObject shapeNode = meshDagPath.node();
    MCallbackId id = MNodeMessage::addAttributeChangedCallback(shapeNode, 
                                    attrChangedCB, data, &status );

    if ( status ) {
        _callbackIds.append( id );
    } else {
        cerr << "MNodeMessage.addCallback failed" << endl;
    }
}


//
// #### initialize
//
//      Set up vertex buffer descriptors and geometry requirements
//
MString
OpenSubdivShaderOverride::initialize(const MInitContext &initContext,
                                     MInitFeedback &initFeedback)
{
    MString empty;

    // Roundabout way of getting positions pulled into our OsdBufferGenerator
    // where we can manage the VBO memory size.
    // Needs to be re-visited, re-factored, optimized, etc.
    {
        MHWRender::MVertexBufferDescriptor positionDesc(
            empty,
            MHWRender::MGeometry::kPosition,
            MHWRender::MGeometry::kFloat,
            3);
        addGeometryRequirement(positionDesc);
    }

    {
        MHWRender::MVertexBufferDescriptor positionDesc(
            "osdPosition",
            MHWRender::MGeometry::kTangent,
            MHWRender::MGeometry::kFloat,
            3);
        positionDesc.setSemanticName("osdPosition");
        addGeometryRequirement(positionDesc);
    }

    if (initFeedback.customData == NULL) {
        OsdMeshData *data = new OsdMeshData(initContext.dagPath);
        initFeedback.customData = data;
    }

    // Add a Maya callback so we can rebuild HBR mesh if topology changes
    addTopologyChangedCallbacks( initContext.dagPath, (OsdMeshData*)initFeedback.customData );

    return MString("OpenSubdivShaderOverride");
}


//
// #### updateDG
//
//      Save pointer to shader so we have access down the line.
//      Call shader to update any attributes it needs to.
//
void
OpenSubdivShaderOverride::updateDG(MObject object)
{
    if (object == MObject::kNullObj)
        return;

    // Save pointer to shader for access from draw()
    _shader = static_cast<OpenSubdivShader*>(
        MPxHwShaderNode::getHwShaderNodePtr(object));

    // Get updated attributes from shader
    if (_shader) {
        _shader->updateAttributes();
    }
}

void
OpenSubdivShaderOverride::updateDevice()
{
    // only place to access GPU device safely
}

void
OpenSubdivShaderOverride::endUpdate()
{
}


//
// #### Override draw method.  
//
// Setup draw state and call osdMeshData methods to setup 
// and refine geometry.  Call to shader to do actual drawing.
//
bool
OpenSubdivShaderOverride::draw(
    MHWRender::MDrawContext &context,
    const MHWRender::MRenderItemList &renderItemList) const
{
    {
        MHWRender::MStateManager *stateMgr = context.getStateManager();
        static const MDepthStencilState * depthState = NULL;
        if (!depthState) {
            MDepthStencilStateDesc desc;
            depthState = stateMgr->acquireDepthStencilState(desc);
        }
        static const MBlendState *blendState = NULL;
        if (!blendState) {
            MBlendStateDesc desc;

            int ntargets = desc.independentBlendEnable ?
                MHWRender::MBlendState::kMaxTargets : 1;

            for (int i = 0; i < ntargets; ++i) {
                desc.targetBlends[i].blendEnable = false;
            }
            blendState = stateMgr->acquireBlendState(desc);
        }

        stateMgr->setDepthStencilState(depthState);
        stateMgr->setBlendState(blendState);
    }

    for (int i = 0; i < renderItemList.length(); i++) 
    {
        const MHWRender::MRenderItem *renderItem = renderItemList.itemAt(i);
        OsdMeshData *data =
            static_cast<OsdMeshData*>(renderItem->customData());
        if (data == NULL) {
            return false;
        }

        // If attributes or topology have changed which affect 
        // the HBR mesh it will be regenerated here.
        data->rebuildHbrMeshIfNeeded(_shader);

        const MHWRender::MVertexBuffer *position = NULL;
        {
            const MHWRender::MGeometry *geometry = renderItem->geometry();
            for (int i = 0; i < geometry->vertexBufferCount(); i++) {
                const MHWRender::MVertexBuffer *vb = geometry->vertexBuffer(i);
                const MHWRender::MVertexBufferDescriptor &vdesc = vb->descriptor();

                if (vdesc.name() == "osdPosition")
                    position = vb;
            }
        }

        // If HBR mesh was regenerated, rebuild FAR mesh factory
        // and recreate OSD draw context
        data->prepare();

        // Refine geometry
        data->updateGeometry(position);

        // Draw patches
        _shader->draw(context, data);
    }

    return true;
}

// -----------------------------------------------------------------------------
// #### OsdBufferGenerator
//
// Vertex buffer generator for OpenSubdiv geometry
//

class OsdBufferGenerator : public MHWRender::MPxVertexBufferGenerator
{
public:
    OsdBufferGenerator() {}
    virtual ~OsdBufferGenerator() {}

#if MAYA_API_VERSION >= 201400
    virtual bool getSourceIndexing(
        const MObject &object,
        MHWRender::MComponentDataIndexing &sourceIndexing) const
    {
        MFnMesh mesh(object);
#else
    virtual bool getSourceIndexing(
        const MDagPath &dagPath,
        MHWRender::MComponentDataIndexing &sourceIndexing) const
    {
        MFnMesh mesh(dagPath.node());
#endif

        MIntArray vertexCount, vertexList;
        mesh.getVertices(vertexCount, vertexList);

        MUintArray &vertices = sourceIndexing.indices();
        for (unsigned int i = 0; i < vertexList.length(); ++i)
            vertices.append((unsigned int)vertexList[i]);

        sourceIndexing.setComponentType(MComponentDataIndexing::kFaceVertex);

        return true;
    }

#if MAYA_API_VERSION >= 201400
    virtual bool getSourceStreams(const MObject &object,
                                  MStringArray &) const
#else
    virtual bool getSourceStreams(const MDagPath &dagPath,
                                  MStringArray &) const
#endif
    {
        return false;
    }

#if MAYA_API_VERSION >= 201350
    virtual void createVertexStream(
#if MAYA_API_VERSION >= 201400
        const MObject &object, 
#else
        const MDagPath &dagPath, 
#endif        
              MVertexBuffer &vertexBuffer,
        const MComponentDataIndexing &targetIndexing,
        const MComponentDataIndexing &,
        const MVertexBufferArray &) const
    {
#else
    virtual void createVertexStream(
        const MDagPath &dagPath, MVertexBuffer &vertexBuffer,
        const MComponentDataIndexing &targetIndexing) const
    {
#endif

#if MAYA_API_VERSION >= 201400
        MFnMesh meshFn(object);
#else
        MFnMesh meshFn(dagPath);
#endif
        int nVertices = meshFn.numVertices();
        MFloatPointArray points;
        meshFn.getPoints(points);

#if MAYA_API_VERSION >= 201350
        float *buffer = static_cast<float*>(vertexBuffer.acquire(nVertices, true));
#else
        float *buffer = static_cast<float*>(vertexBuffer.acquire(nVertices));
#endif
        float *dst = buffer;
        for (int i = 0; i < nVertices; ++i) {
            *dst++ = points[i].x;
            *dst++ = points[i].y;
            *dst++ = points[i].z;
        }
        vertexBuffer.commit(buffer);
    }

    static MPxVertexBufferGenerator *positionBufferCreator()
    {
        return new OsdBufferGenerator();
    }
private:
};

//---------------------------------------------------------------------------
// Plugin Registration
//---------------------------------------------------------------------------
MStatus
initializePlugin(MObject obj)
{
    MStatus status = MS::kSuccess;
    MFnPlugin plugin(obj, "Pixar", "3.0", "Any");  // vendor,version,apiversion

    MString swatchName = MHWShaderSwatchGenerator::initialize();
    MString userClassify("shader/surface/utility/:"
                         "drawdb/shader/surface/OpenSubdivShader:"
                         "swatch/"+swatchName);

#if not defined(__APPLE__)
    glewInit();
#endif

    g_cpuComputeController = new OpenSubdiv::OsdCpuComputeController();

#ifdef OPENSUBDIV_HAS_OPENMP
    g_ompComputeController = new OpenSubdiv::OsdOmpComputeController();
#endif

#ifdef OPENSUBDIV_HAS_CUDA
    cudaInit();
    g_cudaComputeController = new OpenSubdiv::OsdCudaComputeController();
#endif

#ifdef OPENSUBDIV_HAS_OPENCL
    if (initCL(&g_clContext, &g_clQueue) == false) {
        MGlobal::displayError("Cannot initialize OpenCL");
        status.perror("OpenCL initialization");
        return MS::kFailure;
    }
    g_clComputeController = new OpenSubdiv::OsdCLComputeController(g_clContext,
                                                                   g_clQueue);
#endif

    // shader node
    status = plugin.registerNode("openSubdivShader",
                                 OpenSubdivShader::id,
                                 &OpenSubdivShader::creator,
                                 &OpenSubdivShader::initialize,
                                 MPxNode::kHwShaderNode,
                                 &userClassify);

    MHWRender::MDrawRegistry::registerVertexBufferGenerator(
        "osdPosition", OsdBufferGenerator::positionBufferCreator);

    // shaderoverride
    status = MHWRender::MDrawRegistry::registerShaderOverrideCreator(
        "drawdb/shader/surface/OpenSubdivShader",
        OpenSubdivShader::drawRegistrantId,
        OpenSubdivShaderOverride::creator);

    return status;
}

MStatus
uninitializePlugin(MObject obj)
{
    MStatus status = MS::kSuccess;

    MFnPlugin plugin(obj);

    plugin.deregisterNode(OpenSubdivShader::id);

    MHWRender::MDrawRegistry::deregisterVertexBufferGenerator("osdPosition");

    MHWRender::MDrawRegistry::deregisterShaderOverrideCreator(
        "drawdb/shader/surface/OpenSubdivShader",
        OpenSubdivShader::drawRegistrantId);

    delete g_cpuComputeController;

#ifdef OPENSUBDIV_HAS_OPENMP
    delete g_ompComputeController;
#endif

#ifdef OPENSUBDIV_HAS_CUDA
    delete g_cudaComputeController;
#endif

#ifdef OPENSUBDIV_HAS_OPENCL
    delete g_clComputeController;
#endif

    return status;
}

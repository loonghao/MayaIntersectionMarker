#include "EmbreeKernel.h"
#include "../utility.h"

#include <glm/glm.hpp>
#include <embree4/rtcore.h>
#include <embree4/rtcore_geometry.h>
#include <embree4/rtcore_scene.h>

#include <maya/MStatus.h>
#include <maya/MMatrix.h>
#include <maya/MFnMesh.h>
#include <maya/MFnDagNode.h>
#include <maya/MItMeshPolygon.h>
#include <maya/MBoundingBox.h>
#include <maya/MPoint.h>
#include <maya/MPointArray.h>
#include <maya/MIntArray.h>
#include <maya/MGlobal.h>
#include <maya/MStatus.h>

#include <stack>
#include <queue>
#include <vector>
#include <cstdint>
#include <functional>
#include <cassert>


/* This function is called by the builder to signal progress and to
 * report memory consumption. */
bool memoryMonitor(void* userPtr, ssize_t bytes, bool post) {
    return true;
}


bool buildProgress (void* userPtr, double f) {
    return true;
}


void splitPrimitive (const RTCBuildPrimitive* prim, unsigned int dim, float pos, RTCBounds* lprim, RTCBounds* rprim, void* userPtr)
{
    assert(dim < 3);
    assert(prim->geomID == 0);
    *(MBoundingBox*) lprim = *(MBoundingBox*) prim;
    *(MBoundingBox*) rprim = *(MBoundingBox*) prim;
    (&lprim->upper_x)[dim] = pos;
    (&rprim->lower_x)[dim] = pos;
}


void errorHandler(void* userPtr, enum RTCError code, const char* str)
{
    printf("Embree Error: ");
    MGlobal::displayError("Embree Error: ");
    switch (code) {
        case RTC_ERROR_NONE:
            MGlobal::displayError("No error has been recorded. str parameter is set to nullptr.\n");
            break;
        case RTC_ERROR_UNKNOWN:
            MGlobal::displayError("An unknown error has occurred. Typically, this error will occur when an error has been generated in the Embree kernel that did not correspond to any of the other error codes.\n");
            break;
        case RTC_ERROR_INVALID_ARGUMENT:
            MGlobal::displayError("An invalid argument was passed.\n");
            break;
        case RTC_ERROR_INVALID_OPERATION:
            MGlobal::displayError("The operation is not allowed for the current state.\n");
            break;
        case RTC_ERROR_OUT_OF_MEMORY:
            MGlobal::displayError("There is not enough memory left to complete the operation.\n");
            break;
        case RTC_ERROR_UNSUPPORTED_CPU:
            MGlobal::displayError("The user tried to run the library on an unsupported CPU. The CPU must support at least SSE2.\n");
            break;
        case RTC_ERROR_CANCELLED:
            MGlobal::displayError("The operation was cancelled by the user.\n");
            break;
    }
    if (str) {
        MGlobal::displayError(str);
    }
}

MStatus EmbreeKernel::build(const MObject& meshObject, const MBoundingBox& bbox, const MMatrix& offsetMatrix)
{
    MStatus status;

    this->device = rtcNewDevice(nullptr);
    if(!this->device)
    {
        MGlobal::displayError("Failed to create Embree device");
        return MStatus::kFailure;
    }

    rtcSetDeviceErrorFunction(this->device, errorHandler, nullptr);

    // this->scene = rtcNewScene(rtcDevice);
    this->bvh = rtcNewBVH(this->device);
    if (!this->bvh) {
        MGlobal::displayError("Failed to create Embree BVH");
        return MStatus::kFailure;
    }

    // store the PrimID to face id and triangle id mapping
    this->triangles.clear();

    // collect all triangles
    std::vector<RTCBuildPrimitive> primitives;
    MItMeshPolygon itPoly(meshObject);
    int i = 0;
    for(; !itPoly.isDone(); itPoly.next()) {

        int numTriangles;
        itPoly.numTriangles(numTriangles);

        for (int triangleId=0; triangleId < numTriangles; ++triangleId) {
            MPointArray points;
            MIntArray vertexList;
            itPoly.getTriangle(triangleId, points, vertexList, MSpace::kObject);
            MBoundingBox bbox;
            bbox.expand(points[0]);
            bbox.expand(points[1]);
            bbox.expand(points[2]);

            RTCBuildPrimitive prim;
            prim.lower_x = (float)bbox.min().x;
            prim.lower_y = (float)bbox.min().y;
            prim.lower_z = (float)bbox.min().z;
            prim.geomID = 0;
            prim.upper_x = (float)bbox.max().x;
            prim.upper_y = (float)bbox.max().y;
            prim.upper_z = (float)bbox.max().z;
            prim.primID = i;
            primitives.push_back(prim);

            TriangleData triangle;
            triangle.vertices[0] = points[0];
            triangle.vertices[1] = points[1];
            triangle.vertices[2] = points[2];
            triangle.faceIndex = itPoly.index();
            triangle.triangleIndex = triangleId;
            this->triangles.push_back(triangle);

            i++;
        }
    }

    // Build BVH
    RTCBuildArguments arguments = rtcDefaultBuildArguments();
    arguments.byteSize               = sizeof(arguments);
    // arguments.buildFlags             = RTC_BUILD_FLAG_NONE;
    arguments.buildFlags             = RTC_BUILD_FLAG_DYNAMIC;
    arguments.buildQuality           = RTC_BUILD_QUALITY_MEDIUM;
    arguments.maxBranchingFactor     = 2;
    arguments.maxDepth               = 1024;
    arguments.sahBlockSize           = 1;
    arguments.minLeafSize            = 1;
    arguments.maxLeafSize            = 8;
    arguments.traversalCost          = 1.0f;
    arguments.intersectionCost       = 1.0f;
    arguments.bvh                    = this->bvh;
    arguments.primitives             = primitives.data();
    arguments.primitiveCount         = primitives.size();
    arguments.primitiveArrayCapacity = primitives.capacity();
    arguments.createNode             = InnerNode::create;
    arguments.setNodeChildren        = InnerNode::setChildren;
    arguments.setNodeBounds          = InnerNode::setBounds;
    arguments.createLeaf             = LeafNode::create;
    arguments.splitPrimitive         = splitPrimitive;
    arguments.buildProgress          = buildProgress;
    arguments.userPtr                = nullptr;
    // arguments.userPtr                = userData.data();

    root = (Node *)rtcBuildBVH(&arguments);
    if (!root) {
        MGlobal::displayError("Failed to build Embree BVH");
        return MStatus::kFailure;
    }

    // for (size_t i=0; i<10; i++)
    // {
    //     /* we recreate the prims array here, as the builders modify this array */
    //     for (size_t j=0; j<prims.size(); j++) prims[j] = prims_i[j];
    // 
    //     std::cout << "iteration " << i << ": building BVH over " << prims.size() << " primitives, " << std::flush;
    //     double t0 = getSeconds();
    //     Node* root = (Node*) rtcBuildBVH(&arguments);
    //     double t1 = getSeconds();
    //     const float sah = root ? root->sah() : 0.0f;
    //     std::cout << 1000.0f*(t1-t0) << "ms, " << 1E-6*double(prims.size())/(t1-t0) << " Mprims/s, sah = " << sah << " [DONE]" << std::endl;
    // }
    // 
    // rtcReleaseBVH(bvh);

    return MStatus::kSuccess;
}



std::vector<TriangleData> EmbreeKernel::queryIntersected(const TriangleData& triangle) const
{
    // TODO: Implement Embree querying
    return std::vector<TriangleData>();
}


K2KIntersection EmbreeKernel::intersectKernelKernel(

    SpatialDivisionKernel& otherKernel

) const {
    MGlobal::displayInfo(MString("Intersecting EmbreeKernel with "));

    std::vector<TriangleData> intersectingA;
    std::vector<TriangleData> intersectingB;

    EmbreeKernel* other = dynamic_cast<EmbreeKernel*>(&otherKernel);
    if (!other) {
        MGlobal::displayError("Failed to cast SpatialDivisionKernel to EmbreeKernel");
        return std::make_pair(intersectingA, intersectingB);
    }

    // FIXME: change this to stackless traversal
    // // Start with root nodes of both BVHs
    // std::stack<NodePair> stack;
    // stack.push({root, other->root});
    // 
    // while (!stack.empty()) {
    // 
    //     NodePair currentPair = stack.top();
    //     stack.pop();
    // 
    //     // Cast the base Node type to their respective subtypes
    //     bool isAInner = currentPair.nodeA->branch() != nullptr;
    //     bool isBInner = currentPair.nodeB->branch() != nullptr;
    //     bool isALeaf  = currentPair.nodeA->leaf()   != nullptr;
    //     bool isBLeaf  = currentPair.nodeB->leaf()   != nullptr;
    // 
    //     Node *nodeA = currentPair.nodeA;
    //     Node *nodeB = currentPair.nodeB;
    // 
    //     // Iterate over the bounds of each node and check for intersections
    //     bool anyIntersected = false;
    //     for (int i = 0; i < 2; ++i)  {
    //         if (anyIntersected){ break; }
    // 
    //         // Assign the bounds for A and B depending on their type
    //         MBoundingBox bboxA, bboxB;
    //         if      (isAInner){ bboxA = nodeA->branch()->bounds[i]; }
    //         else if  (isALeaf){ bboxA = nodeA->leaf()->bounds; }
    //         else { throw std::runtime_error("Node A is neither a leaf nor an inner node"); }
    // 
    //         for (int j = 0; j < 2; ++j) {
    //             if      (isBInner){ bboxB = nodeB->branch()->bounds[i]; }
    //             else if  (isBLeaf){ bboxB = nodeB->leaf()->bounds; }
    //             else throw std::runtime_error("Node B is neither a leaf nor an inner node");
    // 
    //             // Check if the bounds intersect
    //             if (intersectBoxBox(bboxA, bboxB)) {
    //                 anyIntersected = true;
    //                 break;
    //             }
    //         }
    //     }
    // 
    //     // Skip if bounding boxes of current nodes do not intersect
    //     if (!anyIntersected) {
    //         continue;
    //     }
    // 
    //     if (isALeaf && isBLeaf) {
    //         LeafNode* leafNodeA = nodeA->leaf();
    //         LeafNode* leafNodeB = nodeB->leaf();
    // 
    //         intersectingA.push_back( this->triangles[leafNodeA->id]);
    //         intersectingB.push_back(other->triangles[leafNodeB->id]);
    // 
    //     } else {
    //         // If one or both nodes are not leaves, push their children onto the stack
    //         if (isAInner) {
    //             InnerNode* innerNodeA = nodeA->branch();
    //             for (Node* child : nodeA->branch()->children) {
    //                 stack.push({child, nodeB});
    //             }
    // 
    //         }
    //         if (isBInner) {
    //             InnerNode* innerNodeB = nodeB->branch();
    // 
    //             for (Node* child : nodeB->branch()->children) {
    //                 stack.push({nodeA, child});
    //             }
    //         }
    //     }
    // }

    return std::make_pair(intersectingA, intersectingB);
}

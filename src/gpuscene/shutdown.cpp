#include "gpuscene/shutdown.h"

#include "gpuscene/billboard_table.h"
#include "gpuscene/bone_table.h"
#include "gpuscene/cluster_table.h"
#include "gpuscene/draw_record_table.h"
#include "gpuscene/illumination_table.h"
#include "gpuscene/instance_buffer.h"
#include "gpuscene/lighting_images.h"
#include "gpuscene/material_table.h"
#include "gpuscene/mesh_parts.h"
#include "gpuscene/object_table.h"
#include "gpuscene/row_table.h"
#include "gpuscene/owned_compact.h"
#include "gpuscene/view_table.h"
#include "gpuscene/window_tables.h"

namespace rvk::gpuscene {

void releaseGpuObjects(d3d9::Device& dev) {
    clusterstore().destroy(&dev);  // (the worker, the arena copies, the table)
    ownedcompact().destroy();
    drawrecords().destroy();
    bonetable().destroy();
    lightingimages().releaseGpu();
    illumtable().releaseGpu();
    meshparts().releaseGpu();
    materialtable().releaseGpu();
    objecttable().releaseGpu();
    rowtable().releaseGpu();
    viewtable().releaseGpu();
    windowtables().releaseGpu();
    bbtable().releaseGpu();
    instances().releaseGpu();
}

}  // namespace rvk::gpuscene

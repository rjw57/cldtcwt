#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include "util/clUtil.h"


#include "DisplayOutput/calculatorInterface.h"
#include <iostream>


CalculatorInterface::CalculatorInterface(cl::Context& context,
                                         const cl::Device& device,
                                         int width, int height)
 : width_(width), height_(height),
   calculator_(context, device, width, height),
   cq_(context, device, CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE),
   greyscaleToRGBA_(context, {device}),
   absToRGBA_(context, {device}),
   imageTexture_(GL_RGBA8, width, height),
   imageTextureCL_(context, CL_MEM_READ_WRITE, 
                   GL_TEXTURE_2D, 0, imageTexture_.getTexture()),
   imageGreyscale_(context, CL_MEM_READ_WRITE, 
                   cl::ImageFormat(CL_LUMINANCE, CL_UNORM_INT8),
                   width, height),
   pboBuffer_(1)
{
    // Set up the subband textures
    for (size_t n = 0; n < numSubbands; ++n) {

        // Create OpenGL texture
        subbandTextures2_[n] = GLTexture(GL_RGBA8, width / 4, height / 4);

        // Add OpenCL link to it
        subbandTextures2CL_[n]
            = GLImage(context, CL_MEM_READ_WRITE, 
                      GL_TEXTURE_2D, 0, 
                      subbandTextures2_[n].getTexture());

        // Create OpenGL texture
        subbandTextures3_[n] = GLTexture(GL_RGBA8, width / 8, height / 8);

        // Add OpenCL link to it
        subbandTextures3CL_[n]
            = GLImage(context, CL_MEM_READ_WRITE, 
                      GL_TEXTURE_2D, 0, 
                      subbandTextures3_[n].getTexture());


    }

    // Set up for the energy map texture
    energyMapTexture_ = GLTexture(GL_RGBA8, 
        calculator_.getEnergyMapLevel2().getImageInfo<CL_IMAGE_WIDTH>(),
        calculator_.getEnergyMapLevel2().getImageInfo<CL_IMAGE_HEIGHT>());

    // Add OpenCL link to it
    energyMapTextureCL_ = GLImage(context, CL_MEM_READ_WRITE, 
                                  GL_TEXTURE_2D, 0, 
                                  energyMapTexture_.getTexture());

}

#include <cstring>

void CalculatorInterface::processImage(const void* data, size_t length)
{
    // Upload using OpenCL, not copying the data into its own memory.  This
    // means we can't use the data until the transfer is done.
    cq_.enqueueWriteImage(imageGreyscale_, 
                          // Don't block
                          CL_FALSE, 
                          // Start corner and size
                          makeCLSizeT<3>({0, 0, 0}), 
                          makeCLSizeT<3>({width_, height_, 1}), 
                          // Stride and data pointer
                          0, 0, data,
                          nullptr, &imageGreyscaleDone_);

    calculator_(imageGreyscale_, {imageGreyscaleDone_});

    // Go over to using the OpenGL objects.  glFinish should already have
    // been called
    std::vector<cl::Memory> glTransferObjs = {imageTextureCL_,
                                              energyMapTextureCL_};
    std::copy(subbandTextures2CL_.begin(), subbandTextures2CL_.end(), 
              std::back_inserter(glTransferObjs));
    std::copy(subbandTextures3CL_.begin(), subbandTextures3CL_.end(), 
              std::back_inserter(glTransferObjs));

    cl::Event glObjsAcquired;
    cq_.enqueueAcquireGLObjects(&glTransferObjs, nullptr, &glObjsAcquired);

    // Convert the input image to RGBA for display
    greyscaleToRGBA_(cq_, imageGreyscale_, imageTextureCL_, 1.0f,
                     {imageGreyscaleDone_, glObjsAcquired}, 
                     &imageTextureCLDone_);

    auto subbands = calculator_.levelOutputs();

    std::vector<cl::Event> subbandsConverted(2*numSubbands);

    // Convert the subbands to absolute images

    // Wait for the level and the GL objects to be acquired
    std::vector<cl::Event> subbandsInput2Ready = {glObjsAcquired};
    std::copy(subbands[0]->done.begin(), subbands[0]->done.end(),
              std::back_inserter(subbandsInput2Ready));

    std::vector<cl::Event> subbandsInput3Ready = {glObjsAcquired};
    std::copy(subbands[1]->done.begin(), subbands[1]->done.end(),
              std::back_inserter(subbandsInput3Ready));
    
    for (size_t n = 0; n < numSubbands; ++n) {

        absToRGBA_(cq_, subbands[0]->sb[n], 
                        subbandTextures2CL_[n], 4.0f, subbandsInput2Ready, 
                        &subbandsConverted[n]);

        absToRGBA_(cq_, subbands[1]->sb[n], 
                        subbandTextures3CL_[n], 4.0f, subbandsInput3Ready, 
                        &subbandsConverted[numSubbands+n]);

    }

    std::vector<cl::Event> energyMapReady = calculator_.keypointLocationEvents();
    energyMapReady.push_back(glObjsAcquired);

    cl::Image2D energyMapInput = calculator_.getEnergyMapLevel2();
    // Convert the energy map
    greyscaleToRGBA_(cq_, energyMapInput,
                          energyMapTextureCL_,
                          10.0f,
                          energyMapReady, &energyMapTextureCLDone_);

    // Stop using the OpenGL objects
    std::vector<cl::Event> releaseEvents = {imageTextureCLDone_,
                                            energyMapTextureCLDone_};
    std::copy(subbandsConverted.begin(), subbandsConverted.end(),
              std::back_inserter(releaseEvents));

    cq_.enqueueReleaseGLObjects(&glTransferObjs,
                                &releaseEvents, &glObjsReady_);
}


bool CalculatorInterface::isDone()
{
    return glObjsReady_.getInfo<CL_EVENT_COMMAND_EXECUTION_STATUS>()
                == CL_COMPLETE;
}


void CalculatorInterface::updateGL(void)
{
    // TODO
}



GLuint CalculatorInterface::getImageTexture()
{
    return imageTexture_.getTexture();
}



GLuint CalculatorInterface::getEnergyMapTexture()
{
    return energyMapTexture_.getTexture();
}



GLuint CalculatorInterface::getSubband2Texture(int subband)
{
    return subbandTextures2_[subband].getTexture();
}



GLuint CalculatorInterface::getSubband3Texture(int subband)
{
    return subbandTextures3_[subband].getTexture();
}


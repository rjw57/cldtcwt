#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>


#include "DisplayOutput/calculatorInterface.h"
#include <iostream>


CalculatorInterface::CalculatorInterface(cl::Context& context,
                                         const cl::Device& device,
                                         int width, int height)
 : imageTexture_(GL_RGBA8, width, height),
   calculator_(context, device, width, height),
   pboBuffer_(1)
{
    // Couple it for CL to use
    imageTextureCL_ 
        = GLImage(context, CL_MEM_READ_WRITE, 
                  GL_TEXTURE_2D, 0, imageTexture_.getTexture());



    
}
#include <cstring>

void CalculatorInterface::processImage(const cv::Mat& input)
{
    // Take the image in BGR unsigned byte format

    // Initialise the PBO buffer for use in data transfer
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pboBuffer_.getBuffer(0));
    glBufferData(GL_PIXEL_UNPACK_BUFFER, input.cols * input.rows * 4,
                                         0, GL_STREAM_DRAW);

    GLvoid* ptr = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

    memcpy(ptr, input.data, 3 * input.cols * input.rows);

    // Upload the texture
	glBindTexture(GL_TEXTURE_2D, imageTexture_.getTexture());
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
    			 input.cols, input.rows, 
    			 GL_BGR, GL_UNSIGNED_BYTE,  // Input format
                 0);

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    // Make sure everything's done before we acquire for use by OpenCL
    glFinish();

}


void CalculatorInterface::updateGL(void)
{
    // TODO
}


GLuint CalculatorInterface::getImageTexture()
{
    return imageTexture_.getTexture();
}

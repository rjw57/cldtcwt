#include <iostream>
#include <fstream>
#include <vector>
#include <tuple>

#define __CL_ENABLE_EXCEPTIONS
#include "CL/cl.hpp"

#include "DTCWT/filterer.h"
#include "util/clUtil.h"
#include <iomanip>

#include <stdexcept>

#include <highgui.h>


#include "KeypointDetector/findMax.h"




int main()
{
    try {

        CLContext context;

        // Ready the command queue on the first device to hand
        cl::CommandQueue cq(context.context, context.devices[0]);

        //-----------------------------------------------------------------
        // Starting test code
        
        FindMax findMax(context.context, context.devices);

        const int width = 20, height = 20;
        // Set up data for the input image
        float data[height][width];
        for (int x = 0; x < width; ++x)
            for (int y = 0; y < height; ++y)
                data[y][x] = 0.0f;
        data[10][5] = 1.0f;
        data[13][12] = 1.0f;
        data[14][12] = 2.0f;
        data[1][1] = 1.0f;


        cl::Image2D inImage = {
            context.context, 
            CL_MEM_READ_WRITE,
            cl::ImageFormat(CL_LUMINANCE, CL_FLOAT), 
            width, height, 0
        };


        writeImage2D(cq, inImage, &data[0][0]);
        cq.finish();
                                       

        int zero = 0;

        cl::Buffer outputs = {
            context.context,
            0,              // Flags
            10 * 2 * sizeof(float) // Size to allocate
        };

        cl::Buffer numOutputs = {
            context.context,
            CL_MEM_READ_WRITE,              // Flags
            sizeof(int) // Size to allocate
        };

        cq.enqueueWriteBuffer(numOutputs, CL_TRUE, 0, sizeof(int), &zero);

        float zerof = 0.0f;

        cl::Image2D zeroImg = {
            context.context, 
            CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
            cl::ImageFormat(CL_LUMINANCE, CL_FLOAT), 
            1, 1, 0,
            &zerof
        };


        findMax(cq, inImage, 1.0,
                    zeroImg, 1.0,
                    zeroImg, 4.0, 
                    0.1f, 
                    outputs, 
                    numOutputs, 0);

        int numOutputsVal;
        cq.enqueueReadBuffer(numOutputs, true, 0, sizeof(int),
                                       &numOutputsVal);

        std::cout << numOutputsVal << " outputs" << std::endl;

        numOutputsVal = (numOutputsVal > 10) ? 10 : numOutputsVal;
        
        if (numOutputsVal > 0) {
            std::vector<float> results(numOutputsVal * 2);
            cq.enqueueReadBuffer(outputs, true, 0, 
                                           numOutputsVal * 2 * sizeof(float),
                                           &results[0]);

            for (auto v: results)
                std::cout << v << std::endl;
        }


    }
    catch (cl::Error err) {
        std::cerr << "Error: " << err.what() << "(" << err.err() << ")"
                  << std::endl;
    }
                     
    return 0;
}



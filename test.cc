#include <iostream>
#include <fstream>
#include <vector>

#define __CL_ENABLE_EXCEPTIONS
#include "cl.hpp"

#include "cv.h"
#include "highgui.h"
#include <stdexcept>

class Filterer {
public:
    Filterer();

    void colDecimateFilter(cl::Image2D& output, cl::Image2D& input, 
                           cl::Buffer& filter, bool pad = false);
    void rowDecimateFilter(cl::Image2D& output, cl::Image2D& input, 
                           cl::Buffer& filter, bool pad = false);

    void colFilter(cl::Image2D& output, cl::Image2D& input, 
                           cl::Buffer& filter);
    void rowFilter(cl::Image2D& output, cl::Image2D& input, 
                           cl::Buffer& filter);

    void quadToComplex(cl::Image2D& out1Re, cl::Image2D& out1Im,
                       cl::Image2D& out2Re, cl::Image2D& out2Im,
                       cl::Image2D& input);

    cl::Image2D createImage2D(cv::Mat& image);
    cl::Image2D createImage2D(int width, int height);
    cv::Mat getImage2D(cl::Image2D);
    cl::Sampler createSampler();

    cl::Buffer createBuffer(const float data[], int length);

private:

    cl::Context context;
    cl::Program program;
    cl::CommandQueue commandQueue;

    cl::Kernel rowDecimateFilterKernel;
    cl::Kernel colDecimateFilterKernel;
    cl::Kernel rowFilterKernel;
    cl::Kernel colFilterKernel;
    cl::Kernel quadToComplexKernel;
};


Filterer::Filterer()
{
    // Retrive platform information
    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);

    if (platforms.size() == 0)
        throw std::runtime_error("No platforms!");

    std::vector<cl::Device> devices;
    platforms[0].getDevices(CL_DEVICE_TYPE_GPU, &devices);

    // Create a context to work in 
    context = cl::Context(devices);


    // Open the program, find its length and read it out
    std::ifstream sourceFile("kernel.cl", std::ios::in | std::ios::ate);
    std::string kernelSource(sourceFile.tellg(), ' ');
    sourceFile.seekg(0, std::ios::beg);
    sourceFile.read(&kernelSource[0], kernelSource.length());

    // Create a program compiled from the source code (read in previously)
    cl::Program::Sources source;
    source.push_back(std::pair<const char*, size_t>(kernelSource.c_str(),
                                kernelSource.length()));
    program = cl::Program(context, source);
    try {
        program.build(devices);
    } catch(cl::Error err) {
        std::cerr << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(devices[0]) 
                  << std::endl;
        throw;
    }

    // Turn these into kernels
    rowDecimateFilterKernel = cl::Kernel(program, "rowDecimateFilter");
    colDecimateFilterKernel = cl::Kernel(program, "colDecimateFilter");
    rowFilterKernel = cl::Kernel(program, "rowFilter");
    colFilterKernel = cl::Kernel(program, "colFilter");
    quadToComplexKernel = cl::Kernel(program, "quadToComplex");

    // Ready the command queue on the first device to hand
    commandQueue = cl::CommandQueue(context, devices[0]);
}
    



cl::Image2D Filterer::createImage2D(cv::Mat& image)
{
    cl::Image2D outImage(context,
                        CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR,
                        cl::ImageFormat(CL_RGBA, CL_FLOAT), 
                        image.cols, image.rows, 0,
                        image.ptr());

    commandQueue.finish();

    return outImage;
}


cl::Image2D Filterer::createImage2D(int width, int height)
{
    cl::Image2D outImage(context,
                       CL_MEM_READ_WRITE,
                       cl::ImageFormat(CL_RGBA, CL_FLOAT), 
                       width, height);
    commandQueue.finish();
    return outImage;
}



cv::Mat Filterer::getImage2D(cl::Image2D image)
{
    // Create a matrix to put the data into
    cv::Mat output(image.getImageInfo<CL_IMAGE_HEIGHT>(), 
                   image.getImageInfo<CL_IMAGE_WIDTH>(),
                   CV_32FC4);


    // Read the data out, blocking until done.  Possible scope for
    // optimisation at a later date.
    //
    cl::size_t<3> origin, extents;
    origin.push_back(0);
    origin.push_back(0);
    origin.push_back(0);
    extents.push_back(output.cols);
    extents.push_back(output.rows);
    extents.push_back(1);


    commandQueue.enqueueReadImage(image, CL_TRUE,
                                  origin, extents,                                                                0, 0,
                                  output.ptr());

    return output;
}

cl::Buffer Filterer::createBuffer(const float data[], int length)
{
    //CL_MEM_COPY_HOST_PTR
    cl::Buffer buffer(context, CL_MEM_READ_WRITE,
                        sizeof(float) * length
                        );

    commandQueue.enqueueWriteBuffer(buffer, CL_TRUE, 0, sizeof(float) * length,
                           const_cast<float*>(data));
    commandQueue.finish();

    return buffer;
}

cl::Sampler Filterer::createSampler()
{
    return cl::Sampler(context, CL_FALSE, CL_ADDRESS_CLAMP,
                       CL_FILTER_NEAREST);
}


void Filterer::rowDecimateFilter(cl::Image2D& output, cl::Image2D& input, 
                                    cl::Buffer& filter, bool pad)
{
    int filterLength = filter.getInfo<CL_MEM_SIZE>() / sizeof(float);

    // Tell the kernel to use the buffers, and how long they are
    rowDecimateFilterKernel.setArg(0, input);         // input
    rowDecimateFilterKernel.setArg(1, createSampler());       // inputStride
    rowDecimateFilterKernel.setArg(2, filter);     // filter
    rowDecimateFilterKernel.setArg(3, filterLength);     // filterLength
    rowDecimateFilterKernel.setArg(4, output);        // output
    rowDecimateFilterKernel.setArg(5, pad? -1 : 0);

    // Output size
    const int rows = output.getImageInfo<CL_IMAGE_HEIGHT>();
    const int cols = output.getImageInfo<CL_IMAGE_WIDTH>() / 2;

    // Execute
    commandQueue.enqueueNDRangeKernel(rowDecimateFilterKernel, cl::NullRange,
                                      cl::NDRange(cols, rows),
                                      cl::NullRange);

    commandQueue.finish();
}


void Filterer::colDecimateFilter(cl::Image2D& output, cl::Image2D& input, 
                                  cl::Buffer& filter, bool pad)
{
    int filterLength = filter.getInfo<CL_MEM_SIZE>() / sizeof(float);

    // Tell the kernel to use the buffers, and how long they are
    colDecimateFilterKernel.setArg(0, input);         // input
    colDecimateFilterKernel.setArg(1, createSampler());       // inputStride
    colDecimateFilterKernel.setArg(2, filter);     // filter
    colDecimateFilterKernel.setArg(3, filterLength);     // filterLength
    colDecimateFilterKernel.setArg(4, output);        // output
    colDecimateFilterKernel.setArg(5, pad? -1 : 0);

    // Output size
    const int rows = output.getImageInfo<CL_IMAGE_HEIGHT>() / 2;
    const int cols = output.getImageInfo<CL_IMAGE_WIDTH>();

    // Execute
    commandQueue.enqueueNDRangeKernel(colDecimateFilterKernel, cl::NullRange,
                                      cl::NDRange(cols, rows),
                                      cl::NullRange);
    commandQueue.finish();
}


void Filterer::rowFilter(cl::Image2D& output, cl::Image2D& input, 
                                    cl::Buffer& filter)
{
    int filterLength = filter.getInfo<CL_MEM_SIZE>() / sizeof(float);

    // Tell the kernel to use the buffers, and how long they are
    rowFilterKernel.setArg(0, input);         // input
    rowFilterKernel.setArg(1, createSampler());       // inputStride
    rowFilterKernel.setArg(2, filter);     // filter
    rowFilterKernel.setArg(3, filterLength);     // filterLength
    rowFilterKernel.setArg(4, output);        // output

    // Output size
    const int rows = output.getImageInfo<CL_IMAGE_HEIGHT>();
    const int cols = output.getImageInfo<CL_IMAGE_WIDTH>();

    // Execute
    commandQueue.enqueueNDRangeKernel(rowFilterKernel, cl::NullRange,
                                      cl::NDRange(cols, rows),
                                      cl::NullRange);

    commandQueue.finish();
}


void Filterer::colFilter(cl::Image2D& output, cl::Image2D& input, 
                                    cl::Buffer& filter)
{
    int filterLength = filter.getInfo<CL_MEM_SIZE>() / sizeof(float);

    // Tell the kernel to use the buffers, and how long they are
    colFilterKernel.setArg(0, input);         // input
    colFilterKernel.setArg(1, createSampler());       // inputStride
    colFilterKernel.setArg(2, filter);     // filter
    colFilterKernel.setArg(3, filterLength);     // filterLength
    colFilterKernel.setArg(4, output);        // output

    // Output size
    const int rows = output.getImageInfo<CL_IMAGE_HEIGHT>();
    const int cols = output.getImageInfo<CL_IMAGE_WIDTH>();

    // Execute
    commandQueue.enqueueNDRangeKernel(colFilterKernel, cl::NullRange,
                                      cl::NDRange(cols, rows),
                                      cl::NullRange);

    commandQueue.finish();
}


void Filterer::quadToComplex(cl::Image2D& out1Re, cl::Image2D& out1Im,
                             cl::Image2D& out2Re, cl::Image2D& out2Im,
                             cl::Image2D& input)
{
    quadToComplexKernel.setArg(0, input);
    quadToComplexKernel.setArg(1, createSampler());
    quadToComplexKernel.setArg(2, out1Re);
    quadToComplexKernel.setArg(3, out1Im);
    quadToComplexKernel.setArg(4, out2Re);
    quadToComplexKernel.setArg(5, out2Im);

    // Output size
    const int rows = out1Re.getImageInfo<CL_IMAGE_HEIGHT>();
    const int cols = out1Re.getImageInfo<CL_IMAGE_WIDTH>();

    // Execute
    commandQueue.enqueueNDRangeKernel(quadToComplexKernel, cl::NullRange,
                                      cl::NDRange(cols, rows),
                                      cl::NullRange);
    commandQueue.finish();
}

struct dtcwtFilters {
    cl::Buffer level1h0;
    cl::Buffer level1h1;
    cl::Buffer level1hbp;

    cl::Buffer level2h0;
    cl::Buffer level2h1;
    cl::Buffer level2hbp;
};

void dtcwtTransform(std::vector<std::vector<cl::Image2D> >& output,
                     Filterer& filterer,
                     cl::Image2D& input, dtcwtFilters& filters,
                     int numLevels, int startLevel = 1)
{
    //std::vector<std::vector<cl::Image2D> > output; 

    cl::Image2D lolo;

         /*   cv::Mat im = filterer.getImage2D(input);
            cv::imshow("Output", im);
            cv::waitKey();*/



    // Go down the tree until the point where we need to start recording
    // the results
    for (int n = 1; n < startLevel; ++n) {

        if (n == 1) {

            int width = input.getImageInfo<CL_IMAGE_WIDTH>();
            int height = input.getImageInfo<CL_IMAGE_HEIGHT>();

            // Pad if an odd number of pixels
            bool padW = width & 1,
                 padH = height & 1;

            // Low-pass filter the rows...
            cl::Image2D lo = 
                filterer.createImage2D(width + padW, height);
            filterer.rowFilter(lo, input, filters.level1h0);

            // ...and the columns
            lolo = 
                filterer.createImage2D(width + padW, height + padH);
            filterer.colFilter(lolo, lo, filters.level1h0);

        } else {

            int width = lolo.getImageInfo<CL_IMAGE_WIDTH>();
            int height = lolo.getImageInfo<CL_IMAGE_HEIGHT>();

            // Pad if a non-multiple of four
            bool padW = (width % 4) != 0,
                 padH = (height % 4) != 0;

            // Low-pass filter the rows...
            cl::Image2D lo = 
                filterer.createImage2D(width / 2 + padW, height);
            filterer.rowDecimateFilter(lo, lolo, filters.level2h0, padW);

            // ...and the columns
            lolo = 
                filterer.createImage2D(width / 2 + padW, height / 2 + padH);
            filterer.colDecimateFilter(lolo, lo, filters.level2h0, padH);
            
        }

    }

    // Transform the image
    for (int n = startLevel; n < (startLevel + numLevels); ++n) {
        cl::Image2D hilo, lohi, bpbp;

        if (n == 1) {

            int width = input.getImageInfo<CL_IMAGE_WIDTH>();
            int height = input.getImageInfo<CL_IMAGE_HEIGHT>();

            // Pad if an odd number of pixels
            bool padW = width & 1,
                 padH = height & 1;

            // Low (row) - high (cols)
            cl::Image2D lo = 
                filterer.createImage2D(width + padW, height);
            filterer.rowFilter(lo, input, filters.level1h0);

            
            lohi =
                filterer.createImage2D(width + padW, height + padH);
            filterer.colFilter(lohi, lo, filters.level1h1);

            // High (row) - low (cols)
            cl::Image2D hi =
                filterer.createImage2D(width + padW, height);
            filterer.rowFilter(hi, input, filters.level1h1);

            hilo =
                filterer.createImage2D(width + padW, height + padH);
            filterer.colFilter(hilo, hi, filters.level1h0);

            // Band pass - band pass
            cl::Image2D bp =
                filterer.createImage2D(width + padW, height);
            filterer.rowFilter(bp, input, filters.level1hbp);

            bpbp =
                filterer.createImage2D(width + padW, height + padH);
            filterer.colFilter(bpbp, bp, filters.level1hbp);


            // Low - low
            lolo = 
                filterer.createImage2D(width + padW, height + padH);
            filterer.colFilter(lolo, lo, filters.level1h0);



        } else {

            /*cv::Mat im = filterer.getImage2D(lolo);
            cv::imshow("Output", im);
            cv::waitKey();*/

            int width = lolo.getImageInfo<CL_IMAGE_WIDTH>();
            int height = lolo.getImageInfo<CL_IMAGE_HEIGHT>();

            // Pad if an odd number of pixels
            bool padW = (width % 4) != 0,
                 padH = (height % 4) != 0;

            // Low (row) - high (cols)
            cl::Image2D lo = 
                filterer.createImage2D(width / 2 + padW, height);
            filterer.rowDecimateFilter(lo, lolo, filters.level2h0, padW);

            lohi =
                filterer.createImage2D(width / 2 + padW, height / 2 + padH);
            filterer.colDecimateFilter(lohi, lo, filters.level2h1, padH);


            // High (row) - low (cols)
            cl::Image2D hi =
                filterer.createImage2D(width / 2 + padW, height);
            filterer.rowDecimateFilter(hi, lolo, filters.level2h1, padW);

            hilo =
                filterer.createImage2D(width / 2 + padW, height / 2 + padH);
            filterer.colDecimateFilter(hilo, hi, filters.level2h0, padH);


            // Band pass - band pass
            cl::Image2D bp =
                filterer.createImage2D(width / 2 + padW, height);
            filterer.rowDecimateFilter(bp, lolo, filters.level2hbp, padW);

            bpbp =
                filterer.createImage2D(width / 2 + padW, height / 2 + padH);
            filterer.colDecimateFilter(bpbp, bp, filters.level2hbp, padH);


            // Low - low
            lolo = 
                filterer.createImage2D(width / 2 + padW, height / 2 + padH);
            filterer.colDecimateFilter(lolo, lo, filters.level2h0, padH);

        }

        output.push_back(std::vector<cl::Image2D>());

        int idx = n - startLevel;
        int width = hilo.getImageInfo<CL_IMAGE_WIDTH>() / 2;
        int height = hilo.getImageInfo<CL_IMAGE_HEIGHT>() / 2;

        for (int n = 0; n < 12; ++n)
            output[idx].push_back(filterer.createImage2D(width, height));


        filterer.quadToComplex(output[idx][2], output[idx][2+6],
                               output[idx][3], output[idx][3+6],
                               lohi);

        filterer.quadToComplex(output[idx][0], output[idx][0+6],
                               output[idx][5], output[idx][5+6],
                               hilo);

        filterer.quadToComplex(output[idx][1], output[idx][1+6],
                               output[idx][4], output[idx][4+6],
                               bpbp);

    }
    //return output;

}


dtcwtFilters createFilters(Filterer& filterer)
{
    const float level1h0[13] = {
       -0.0018,
             0,
        0.0223,
       -0.0469,
       -0.0482,
        0.2969,
        0.5555,
        0.2969,
       -0.0482,
       -0.0469,
        0.0223,
             0,
       -0.0018
    };

    const float level1h1[19] = {
       -0.0001,
             0,
        0.0013,
       -0.0019,
       -0.0072,
        0.0239,
        0.0556,
       -0.0517,
       -0.2998,
        0.5594,
       -0.2998,
       -0.0517,
        0.0556,
        0.0239,
       -0.0072,
       -0.0019,
        0.0013,
             0,
       -0.0001
    };

    const float level1hbp[19] = {
       -0.0004,
       -0.0006,
       -0.0001,
        0.0042,
        0.0082,
       -0.0074,
       -0.0615,
       -0.1482,
       -0.1171,
        0.6529,
       -0.1171,
       -0.1482,
       -0.0615,
       -0.0074,
        0.0082,
        0.0042,
       -0.0001,
       -0.0006,
       -0.0004
    };


    const float level2h0[14] = {
       -0.0046,
       -0.0054,
        0.0170,
        0.0238,
       -0.1067,
        0.0119,
        0.5688,
        0.7561,
        0.2753,
       -0.1172,
       -0.0389,
        0.0347,
       -0.0039,
        0.0033
    };

    const float level2h1[14] = {
       -0.0033,
       -0.0039,
       -0.0347,
       -0.0389,
        0.1172,
        0.2753,
       -0.7561,
        0.5688,
       -0.0119,
       -0.1067,
       -0.0238,
        0.0170,
        0.0054,
       -0.0046
    };

    const float level2hbp[14] = {
       -0.0028,
       -0.0004,
        0.0210,
        0.0614,
        0.1732,
       -0.0448,
       -0.8381,
        0.4368,
        0.2627,
       -0.0076,
       -0.0264,
       -0.0255,
       -0.0096,
       -0.0000
    };

    dtcwtFilters filters;
    filters.level1h0 = filterer.createBuffer(level1h0, 13);
    filters.level1h1 = filterer.createBuffer(level1h1, 19);
    filters.level1hbp = filterer.createBuffer(level1hbp, 19);

    filters.level2h0 = filterer.createBuffer(level2h0, 14);
    filters.level2h1 = filterer.createBuffer(level2h1, 14);
    filters.level2hbp = filterer.createBuffer(level2hbp, 14);

    return filters;
}


int main()
{
    try {
        std::string displays[] = {"S1", "S2", "S3", "S4", "S5", "S6"};

        // Read the image (forcing to RGB), and convert it to floats ("1")
        cv::Mat inImage = cv::imread("circle.bmp", 1);

        // Open the camera for reading
        cv::VideoCapture videoIn(0);
        videoIn.set(CV_CAP_PROP_FRAME_WIDTH, 320);
        videoIn.set(CV_CAP_PROP_FRAME_HEIGHT, 240);


        Filterer filterer;

        dtcwtFilters filters = createFilters(filterer);
        for (int n = 0; n < 6; ++n)
            cv::namedWindow(displays[n], CV_WINDOW_NORMAL);
       
        cv::Mat vidImage;
        cv::Mat outImage;

        int x = 0;
        int numLevels = 4;
        while (1) {
            videoIn >> vidImage;
            //vidImage = inImage;

            cv::Mat inputTmp;
            cv::Mat inputTmp2;
            vidImage.convertTo(inputTmp, CV_32F);
            cvtColor(inputTmp, inputTmp2, CV_RGB2GRAY);
            cv::Mat input(vidImage.size(), CV_32FC4);

            // Working in BGRA (for ref.)
            // Now, put it into 4 channels
            int fromTo[] = {0,0, 0,1, 0,2, -1,3};
            cv::mixChannels(&inputTmp2, 1, &input, 1, fromTo, 4);

            // Send to the graphics card
            cl::Image2D img(filterer.createImage2D(input));

            // Get results
            std::vector<std::vector<cl::Image2D> > results;
            dtcwtTransform(results, filterer, img, filters, numLevels, 2);

            const int l = 0;
            int width = results[l][0].getImageInfo<CL_IMAGE_WIDTH>();
            int height = results[l][0].getImageInfo<CL_IMAGE_HEIGHT>();

            cv::Mat disp(height, width, CV_32FC4);

            // Read them out
            for (int n = 0; n < 6; ++n) {
                cv::Mat re = filterer.getImage2D(results[l][n]);
                cv::Mat im = filterer.getImage2D(results[l][n+6]);
                //cv::Mat outArea = disp.colRange(n*width, (n+1)*width-1);
                cv::sqrt(re.mul(re) + im.mul(im), disp);
                cv::imshow(displays[n], disp / 64.0f);
            }

            // Display
            std::cout << "Displayed! " << x++ <<  std::endl;
            cv::waitKey(1);
        }

/*
        for (int n = 0; n < numLevels; ++n) {
            for (int m = 0; m < 6; ++m) {
                cv::Mat re = filterer.getImage2D(results[n][m]);
                cv::Mat im = filterer.getImage2D(results[n][m+6]);

                //cv::imshow("Output", filteredImage + 0.5f);
            
                cv::imshow("Output", re);
                cv::waitKey();

                cv::imshow("Output", im);
                cv::waitKey();
                cv::imwrite("out.bmp", re);

            }
        }
*/
    }
    catch (cl::Error err) {
        std::cerr << "Error: " << err.what() << "(" << err.err() << ")"
                  << std::endl;
    }

                     
    return 0;
}




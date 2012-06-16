#include "dtcwt.h"

#include "clUtil.h"

Filters createLevel1Filters(cl::Context& context, 
                            cl::CommandQueue& commandQueue);
Filters createLevel2Filters(cl::Context& context, 
                            cl::CommandQueue& commandQueue);

static size_t decimateDim(size_t inSize)
{
    // Decimate the size of a dimension by a factor of two.  If this gives
    // a non-even number, pad so it is.
    
    bool pad = (inSize % 4) != 0;

    return inSize / 2 + (pad? 1 : 0);
}




DtcwtOutput::DtcwtOutput(const DtcwtTemps& env)
{
    for (int l = env.startLevel; l < env.numLevels; ++l) {

        const cl::Image2D& baseImage = env.levelTemps[l].lolo;

        const size_t width = baseImage.getImageInfo<CL_IMAGE_WIDTH>() / 2,
                    height = baseImage.getImageInfo<CL_IMAGE_HEIGHT>() / 2;

        LevelOutput sbs;

        // Create all the complex images at the right size
        for (auto& sb: sbs.sb)
            sb = {env.context_, 0, {CL_RG, CL_FLOAT}, width, height};

        // Add another set of outputs
        subbands.push_back(sbs);
        

    }
}





Dtcwt::Dtcwt(cl::Context& context, const std::vector<cl::Device>& devices,
             cl::CommandQueue commandQueue) : 
    context_ { context },
    level1_ (createLevel1Filters(context_, commandQueue)),
    leveln_ (createLevel2Filters(context_, commandQueue)),
    h0x { context, devices, level1_.h0, Filter::x },
    h0y { context, devices, level1_.h0, Filter::y },
    h1x { context, devices, level1_.h1, Filter::x },
    h1y { context, devices, level1_.h1, Filter::y },
    hbpx { context, devices, level1_.hbp, Filter::x },
    hbpy { context, devices, level1_.hbp, Filter::y },
    g0x { context, devices, leveln_.h0, DecimateFilter::x },
    g0y { context, devices, leveln_.h0, DecimateFilter::y },
    g1x { context, devices, leveln_.h1, DecimateFilter::x, true },
                      // True because we want to swap the trees over
    g1y { context, devices, leveln_.h1, DecimateFilter::y, true },
    gbpx { context, devices, leveln_.hbp, DecimateFilter::x, true },
    gbpy { context, devices, leveln_.hbp, DecimateFilter::y, true },
    quadToComplex(context, devices)
{}



// Create the set of images etc needed to perform a DTCWT calculation
DtcwtTemps Dtcwt::createContext(size_t imageWidth, size_t imageHeight, 
                                  size_t numLevels, size_t startLevel)
{
    DtcwtTemps c;

    // Copy settings to the saved structure
    c.context_ = context_;

    c.width = imageWidth;
    c.height = imageHeight;

    c.numLevels = numLevels;
    c.startLevel = startLevel;

    // Make space in advance for the temps
    c.levelTemps.reserve(numLevels);

    // Allocate space on the graphics card for each of the levels
    
    // 1st level
    size_t width = imageWidth + imageWidth % 2;
    size_t height = imageHeight + imageHeight % 2;

    for (int l = 0; l < numLevels; ++l) {

        // Decimate if we're beyond the first stage, otherwise just make
        // sure we have even width/height
        size_t newWidth = 
            (l == 0)? width + width % 2
                    : decimateDim(width);

        size_t newHeight =
            (l == 0)? height + height % 2
                    : decimateDim(height);

        c.levelTemps.push_back(LevelTemps());

        // Temps that will be needed whether there's an output or not
        c.levelTemps.back().lo
            = createImage2D(context_, width, newHeight);
        c.levelTemps.back().lolo
            = createImage2D(context_, newWidth, newHeight);

        // Temps only needed when producing subband outputs
        if  (l >= startLevel) {
            c.levelTemps.back().hi
                = createImage2D(context_, width, newHeight);
            c.levelTemps.back().bp
                = createImage2D(context_, width, newHeight);

            c.levelTemps.back().lohi
                = createImage2D(context_, newWidth, newHeight);
            c.levelTemps.back().hilo
                = createImage2D(context_, newWidth, newHeight);
            c.levelTemps.back().bpbp
                = createImage2D(context_, newWidth, newHeight);
        }

        width = newWidth;
        height = newHeight;
    }
   
    return c;
}



void Dtcwt::operator() (cl::CommandQueue& commandQueue,
                        cl::Image2D& image, 
                        DtcwtTemps& env,
                        DtcwtOutput& subbandOutputs)
{
    for (int l = 0; l < env.numLevels; ++l) {

        if (l == 0) {

            filter(commandQueue, image, {},
                   env.levelTemps[l], 
                   (env.startLevel == 0) ? 
                       &subbandOutputs.subbands[0]
                     : nullptr);

        } else {

            decimateFilter(commandQueue, 
                           env.levelTemps[l-1].lolo, 
                               {env.levelTemps[l-1].loloDone},
                           env.levelTemps[l], 
                           (l >= env.startLevel) ? 
                               &subbandOutputs.subbands[l - env.startLevel]
                             : nullptr);

        }

    }

}




void Dtcwt::filter(cl::CommandQueue& commandQueue,
                   cl::Image2D& xx, 
                   const std::vector<cl::Event>& xxEvents,
                   LevelTemps& levelTemps, LevelOutput* subbands)
{
    // Apply the non-decimating, special low pass filters that must be needed
    h0y(commandQueue, xx, levelTemps.lo, 
        xxEvents, &levelTemps.loDone);

    h0x(commandQueue, levelTemps.lo, levelTemps.lolo,
        {levelTemps.loDone}, &levelTemps.loloDone);

    // If we've been given subbands to output to, we need to do more work:
    if (subbands) {

        // Produce both the other vertically-filtered versions
        h1y(commandQueue, xx, levelTemps.hi,
            xxEvents, &levelTemps.hiDone);

        hbpy(commandQueue, xx, levelTemps.bp,
            xxEvents, &levelTemps.bpDone);

        // High pass the images that had been low-passed the other way
        h0x(commandQueue, levelTemps.hi, levelTemps.lohi,
            {levelTemps.hiDone}, &levelTemps.lohiDone);

        h1x(commandQueue, levelTemps.lo, levelTemps.hilo,
            {levelTemps.loDone}, &levelTemps.hiloDone);

        hbpx(commandQueue, levelTemps.bp, levelTemps.bpbp,
            {levelTemps.bpDone}, &levelTemps.bpbpDone);

        // Create events that, when all done signify everything about this stage
        // is complete
        subbands->done = std::vector<cl::Event>(3);

        // ...and generate subband outputs.
        quadToComplex(commandQueue, levelTemps.lohi, 
                      subbands->sb[0], subbands->sb[5],
                      {levelTemps.lohiDone}, &subbands->done[0]); 

        quadToComplex(commandQueue, levelTemps.hilo, 
                      subbands->sb[2], subbands->sb[3],
                      {levelTemps.hiloDone}, &subbands->done[1]); 

        quadToComplex(commandQueue, levelTemps.bpbp, 
                      subbands->sb[1], subbands->sb[4],
                      {levelTemps.bpbpDone}, &subbands->done[2]); 
 
    }
}


void Dtcwt::decimateFilter(cl::CommandQueue& commandQueue,
                           cl::Image2D& xx, 
                           const std::vector<cl::Event>& xxEvents,
                           LevelTemps& levelTemps, LevelOutput* subbands)
{
    // Apply the non-decimating, special low pass filters that must be needed
    g0y(commandQueue, xx, levelTemps.lo, 
        xxEvents, &levelTemps.loDone);

    g0x(commandQueue, levelTemps.lo, levelTemps.lolo,
        {levelTemps.loDone}, &levelTemps.loloDone);


    // If we've been given subbands to output to, we need to do more work:
    if (subbands) {

        // Produce both the other vertically-filtered versions
        g1y(commandQueue, xx, levelTemps.hi,
            xxEvents, &levelTemps.hiDone);

        gbpy(commandQueue, xx, levelTemps.bp,
            xxEvents, &levelTemps.bpDone);

        // High pass the images that had been low-passed the other way
        g0x(commandQueue, levelTemps.hi, levelTemps.lohi,
            {levelTemps.hiDone}, &levelTemps.lohiDone);

        g1x(commandQueue, levelTemps.lo, levelTemps.hilo,
            {levelTemps.loDone}, &levelTemps.hiloDone);

        gbpx(commandQueue, levelTemps.bp, levelTemps.bpbp,
            {levelTemps.bpDone}, &levelTemps.bpbpDone);

        // Create events that, when all done signify everything about this stage
        // is complete
        subbands->done = std::vector<cl::Event>(3);

        // ...and generate subband outputs.
        quadToComplex(commandQueue, levelTemps.lohi, 
                      subbands->sb[0], subbands->sb[5],
                      {levelTemps.lohiDone}, &subbands->done[0]); 

        quadToComplex(commandQueue, levelTemps.hilo, 
                      subbands->sb[2], subbands->sb[3],
                      {levelTemps.hiloDone}, &subbands->done[1]); 

        quadToComplex(commandQueue, levelTemps.bpbp, 
                      subbands->sb[1], subbands->sb[4],
                      {levelTemps.bpbpDone}, &subbands->done[2]); 
    }
}



#include <iostream>



EnergyMap::EnergyMap(cl::Context& context,
                     const std::vector<cl::Device>& devices)
   : context_(context)
{
    // The OpenCL kernel:
    const std::string sourceCode = 
        "__kernel void energyMap(__read_only image2d_t sb0,"
                                "__read_only image2d_t sb1,"
                                "__read_only image2d_t sb2,"
                                "__read_only image2d_t sb3,"
                                "__read_only image2d_t sb4,"
                                "__read_only image2d_t sb5,"
                                "__write_only image2d_t out)"
        "{"
            "sampler_t s ="
                "CLK_NORMALIZED_COORDS_FALSE"
                "| CLK_ADDRESS_CLAMP"
                "| CLK_FILTER_NEAREST;"

            "int x = get_global_id(0);"
            "int y = get_global_id(1);"

            "if (x < get_image_width(out)"
             "&& y < get_image_height(out)) {"

                // Sample each subband
                "float2 h0 = read_imagef(sb0, s, (int2) (x,y)).s01;"
                "float2 h1 = read_imagef(sb1, s, (int2) (x,y)).s01;"
                "float2 h2 = read_imagef(sb2, s, (int2) (x,y)).s01;"
                "float2 h3 = read_imagef(sb3, s, (int2) (x,y)).s01;"
                "float2 h4 = read_imagef(sb4, s, (int2) (x,y)).s01;"
                "float2 h5 = read_imagef(sb5, s, (int2) (x,y)).s01;"

                // Convert to absolute (still squared, because it's more
                // convenient)
                "float abs_h0_2 = h0.s0 * h0.s0 + h0.s1 * h0.s1;"
                "float abs_h1_2 = h1.s0 * h1.s0 + h1.s1 * h1.s1;"
                "float abs_h2_2 = h2.s0 * h2.s0 + h2.s1 * h2.s1;"
                "float abs_h3_2 = h3.s0 * h3.s0 + h3.s1 * h3.s1;"
                "float abs_h4_2 = h4.s0 * h4.s0 + h4.s1 * h4.s1;"
                "float abs_h5_2 = h5.s0 * h5.s0 + h5.s1 * h5.s1;"

                // Calculate result
                "float result ="
                    "(  sqrt(abs_h0_2 * abs_h3_2) "
                    " + sqrt(abs_h1_2 * abs_h4_2) "
                    " + sqrt(abs_h2_2 * abs_h5_2))"
                    "/"
                    "sqrt(0.001 + "
                    "   1.5 * (  abs_h0_2 + abs_h1_2 + abs_h2_2"
                               " + abs_h3_2 + abs_h4_2 + abs_h5_2));" 

                // Produce output
                "write_imagef(out, (int2) (x, y), result);"

            "}"

        "}";

    // Bundle the code up
    cl::Program::Sources source;
    source.push_back(std::make_pair(sourceCode.c_str(), sourceCode.length()));

    // Compile it...
    cl::Program program(context, source);

    try {
        program.build(devices);
    } catch(cl::Error err) {
	    std::cerr 
		    << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(devices[0])
		    << std::endl;
	    throw;
    } 
        
    // ...and extract the useful part, viz the kernel
    kernel_ = cl::Kernel(program, "energyMap");

}


static int roundWGs(int l, int lWG)
{
    return lWG * (l / lWG + ((l % lWG) ? 1 : 0)); 
}


void EnergyMap::operator() (cl::CommandQueue& commandQueue,
                            const LevelOutput& levelOutput,
                            cl::Image2D& energyMap,
                            cl::Event* doneEvent)
{
    // Set up all the arguments to the kernel
    for (int n = 0; n < levelOutput.sb.size(); ++n)
        kernel_.setArg(n, levelOutput.sb[n]);

    kernel_.setArg(levelOutput.sb.size(), energyMap);

    const size_t wgSize = 16;

    cl::NDRange globalSize = {
        roundWGs(energyMap.getImageInfo<CL_IMAGE_WIDTH>(), wgSize),
        roundWGs(energyMap.getImageInfo<CL_IMAGE_HEIGHT>(), wgSize)
    };

    // Execute
    commandQueue.enqueueNDRangeKernel(kernel_, cl::NullRange,
                                      globalSize,
                                      {wgSize, wgSize},
                                      &levelOutput.done, doneEvent);
}




Filters createLevel1Filters(cl::Context& context, 
                            cl::CommandQueue& commandQueue)
{
    Filters level1;

    level1.h0 = createBuffer(context, commandQueue, { 
          -0.001757812500000,
           0.000000000000000,
           0.022265625000000,
          -0.046875000000000,
          -0.048242187500000,
           0.296875000000000,
           0.555468750000000,
           0.296875000000000,
          -0.048242187500000,
          -0.046875000000000,
           0.022265625000000,
           0.000000000000000,
          -0.001757812500000
    });

    level1.h1 = createBuffer(context, commandQueue, { 
          -0.000070626395089,
           0.000000000000000,
           0.001341901506696,
          -0.001883370535714,
          -0.007156808035714,
           0.023856026785714,
           0.055643136160714,
          -0.051688058035714,
          -0.299757603236607,
           0.559430803571429,
          -0.299757603236607,
          -0.051688058035714,
           0.055643136160714,
           0.023856026785714,
          -0.007156808035714,
          -0.001883370535714,
           0.001341901506696,
           0.000000000000000,
          -0.000070626395089
    } );
    
    level1.hbp = createBuffer(context, commandQueue, { 
          -3.68250025673202e-04,
          -6.22253585579744e-04,
          -7.81782479825950e-05,
           4.18582084706810e-03,
           8.19178717888364e-03,
          -7.42327402480263e-03,
          -6.15384268799117e-02,
          -1.48158230911691e-01,
          -1.17076301639216e-01,
           6.52908215843590e-01,
          -1.17076301639216e-01,
          -1.48158230911691e-01,
          -6.15384268799117e-02,
          -7.42327402480263e-03,
           8.19178717888364e-03,
           4.18582084706810e-03,
          -7.81782479825949e-05,
          -6.22253585579744e-04,
          -3.68250025673202e-04
    } );

    return level1;
}




Filters createLevel2Filters(cl::Context& context, 
                            cl::CommandQueue& commandQueue)
{
    Filters level2;

    level2.h0 = createBuffer(context, commandQueue, {
          -0.00455689562847549,
          -0.00543947593727412,
           0.01702522388155399,
           0.02382538479492030,
          -0.10671180468666540,
           0.01186609203379700,
           0.56881042071212273,
           0.75614564389252248,
           0.27529538466888204,
          -0.11720388769911527,
          -0.03887280126882779,
           0.03466034684485349,
          -0.00388321199915849,
           0.00325314276365318
    } );

    level2.h1 = createBuffer(context, commandQueue, {
          -0.00325314276365318,
          -0.00388321199915849,
          -0.03466034684485349,
          -0.03887280126882779,
           0.11720388769911527,
           0.27529538466888204,
          -0.75614564389252248,
           0.56881042071212273,
          -0.01186609203379700,
          -0.10671180468666540,
          -0.02382538479492030,
           0.01702522388155399,
           0.00543947593727412,
          -0.00455689562847549
    } );

    level2.hbp = createBuffer(context, commandQueue, {
          -2.77165349347537e-03,
          -4.32919303381105e-04,
           2.10100577283097e-02,
           6.14446533755929e-02,
           1.73241472867428e-01,
          -4.47647940175083e-02,
          -8.38137840090472e-01,
           4.36787385780317e-01,
           2.62691880616686e-01,
          -7.62474758151248e-03,
          -2.63685613793659e-02,
          -2.54554351814246e-02,
          -9.59514305416110e-03,
          -2.43562670333119e-05
    } );

    return level2;
}

















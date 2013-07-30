// Copyright (C) 2013 Timothy Gale
typedef float2 Complex;

typedef struct {
    Complex a, b;
} ComplexPair;


// Interpolate in the upper-left and lower-right (matrix notation) using the 
// four complex coefficients provided.  These coefficients are upper left,
// upper right, lower left, lower right.  They are rotated by 180 degrees for
// the lower-right region and complex-conjugated.
ComplexPair interpDiagonallySymmetricULLR(const __local Complex* centreY, 
                                          size_t stride,
                                          const Complex coefficients[4]);

// Interpolate in the upper-right and lower-left (matrix notation) using the 
// four complex coefficients provided.  These coefficients are upper left,
// upper right, lower left, lower right.  They are rotated by 180 degrees for
// the lower-left region and complex-conjugated.
ComplexPair interpDiagonallySymmetricURLL(const __local Complex* centreY, 
                                          size_t stride,
                                          const Complex coefficients[4]);


// Complex operations
Complex complexMul(Complex v1, Complex v2);
Complex complexConj(Complex v1);

// Load a rectangular region from a floating-point image
void readImageRegionToShared(const __global float2* input,
                unsigned int stride,
                unsigned int padding,
                uint2 inSize,
                int2 regionStart,
                int2 regionSize, 
                __local volatile Complex* output);







// Parameters: WG_SIZE_X, WG_SIZE_Y need to be set for the work group size.
// POS_LEN should be the number of floats to make the output structure.
__kernel __attribute__((reqd_work_group_size(WG_SIZE_X, WG_SIZE_Y, 1)))
void interpMap(const __global float2* sb,
               const unsigned int sbStart,
               const unsigned int sbPitch,
               const unsigned int sbStride,
               const unsigned int sbPadding,
               const unsigned int sbWidth,
               const unsigned int sbHeight,
               __write_only image2d_t output)
{
    //  Angles (radians) of the subband orientations
    const float subbandDirections[6] = {
        -0.2606,  -0.7854,  -1.3102,   4.4518,   3.9270,   3.4022
    };

    // Generated by coeffs.m.  
    const float2 interpCoeffs[6][4] = {
        {
            (Complex) (-0.005240f,0.015359f),
            (Complex) (-0.292823f,-0.065229f),
            (Complex) (0.035089f,0.000000f),
            (Complex) (0.070947f,0.644792f)
        },
        {
            (Complex) (-0.205471f,0.025978f),
            (Complex) (0.500000f,0.000000f),
            (Complex) (0.085786f,0.000000f),
            (Complex) (-0.205471f,-0.025978f)
        },
        {
            (Complex) (0.070947f,-0.644792f),
            (Complex) (-0.292823f,0.065229f),
            (Complex) (0.035089f,0.000000f),
            (Complex) (-0.005240f,-0.015359f)
        },
        {
            (Complex) (-0.292823f,-0.065229f),
            (Complex) (0.070947f,0.644792f),
            (Complex) (-0.005240f,0.015359f),
            (Complex) (0.035089f,0.000000f)
        },
        {
            (Complex) (0.500000f,0.000000f),
            (Complex) (-0.205471f,-0.025978f),
            (Complex) (-0.205471f,0.025978f),
            (Complex) (0.085786f,0.000000f)
        },
        {
            (Complex) (-0.292823f,0.065229f),
            (Complex) (-0.005240f,-0.015359f),
            (Complex) (0.070947f,-0.644792f),
            (Complex) (0.035089f,0.000000f)
        }
    };


    int2 g = (int2) (get_global_id(0), get_global_id(1));
    int2 l = (int2) (get_local_id(0), get_local_id(1));

    // Storage for the subband values
    __local Complex sbVals[WG_SIZE_Y+2][WG_SIZE_X+2];

    int2 regionStart = (int2)
        (get_group_id(0) * get_local_size(0) - 1,
         get_group_id(1) * get_local_size(1) - 1);

    int2 regionSize = (int2) (WG_SIZE_X+2, WG_SIZE_Y+2);

    float2 slp[6];
    float slpLen[6];

    // For each subband
    for (int n = 0; n < 6; ++n) {

        readImageRegionToShared(sb + sbStart + n * sbPitch, 
                                sbStride, sbPadding, 
                                (uint2) (sbWidth, sbHeight),
                                regionStart, regionSize, 
                                &sbVals[0][0]);

        // Make sure we don't start using values until they're valid
        barrier(CLK_LOCAL_MEM_FENCE);

        ComplexPair y; 

        if (n < 3) 
            y = interpDiagonallySymmetricURLL(&sbVals[l.y+1][l.x+1], 
                                              regionSize.x,
                                              interpCoeffs[n]);
        else 
            y = interpDiagonallySymmetricULLR(&sbVals[l.y+1][l.x+1], 
                                              regionSize.x,
                                              interpCoeffs[n]);

        // Calculate weights for the two phases, normalised to add to 1
        float w[2] = {dot(y.a, y.a), dot(y.b, y.b)};
        float sumw = w[0] + w[1];
        w[0] /= sumw;
        w[1] /= sumw;

        // Find the rotations between the three sampling points
        y.a = complexMul(sbVals[l.y+1][l.x+1], complexConj(y.a));
        y.b = complexMul(y.b, complexConj(sbVals[l.y+1][l.x+1]));

        float dphase1 = atan2(y.a.y, y.a.x);
        float dphase2 = atan2(y.b.y, y.b.x);

        

        // Absolute value of angular frequency
        const float absw = 3.1623f * M_PI_F / 2.15f;

        // Difference from original direction (in rad)
        float phase = subbandDirections[n] - (w[0] * dphase1 + w[1] * dphase2) / absw;

        // Add magnitude adjustment thingy here to cope with phases that 
        // have gone too far
        slpLen[n] = length(sbVals[l.y+1][l.x+1]);

        const float taperStart = 90.f / 180.f * M_PI_F,
                    taperEnd   = 150.f / 180.f * M_PI_F;

        // Scale down if passing through the taper regions
        slpLen[n] *= 
        w[0] * 
        clamp((taperEnd - fabs(dphase1)) / (taperEnd - taperStart), 0.f, 1.f)
        + w[1] * 
        clamp((taperEnd - fabs(dphase2)) / (taperEnd - taperStart), 0.f, 1.f);

        // Convert to cartesian
        float xComp, yComp;
        yComp = sincos(phase, &xComp);

        slp[n] = slpLen[n] * (float2) (xComp, yComp);

        // Make sure values aren't overwritten while they might still be
        // being used
        barrier(CLK_LOCAL_MEM_FENCE);

    }

    if (all(g < get_image_dim(output))) {

        float energy = 0;

        for (size_t s1 = 0; s1 < 6; ++s1) {
            for (size_t s2 = 0; s2 < 6; ++s2) {
                energy += 
                    fabs(slp[s1].x * slp[s2].y - slp[s2].x * slp[s1].y)
                        / (fmax(slpLen[s1], slpLen[s2]) + 1.e-9f);
            }
        }

        write_imagef(output, g, energy / 7.5f);
    }
                 
}



void readImageRegionToShared(const __global float2* input,
                unsigned int stride,
                unsigned int padding,
                uint2 inSize,
                int2 regionStart,
                int2 regionSize, 
                __local volatile Complex* output)
{
    // Take a region of regionSize, and load it into local memory with the
    // while workgroup.  Memory is laid out reading along the direction of
    // x.

    // We'll extract a rectangle the size of a workgroup each time.

    // The position within the workgroup
    int2 localPos = (int2) (get_local_id(0), get_local_id(1));

    // Loop over the rectangles
    for (int x = 0; x < regionSize.x; x += get_local_size(0)) {
        for (int y = 0; y < regionSize.y; y += get_local_size(1)) {

            int2 readPosOffset = (int2) (x,y) + localPos;

            bool inRegion = all(readPosOffset < regionSize);

            // Make sure we are still in the rectangular region asked for
            if (inRegion) {
                
                int2 pos = regionStart + readPosOffset;
                
                bool inImage = all((int2) (0, 0) <= pos) 
                                & all(pos < convert_int2(inSize));

                output[readPosOffset.y * regionSize.x + readPosOffset.x]
                    = inImage? 
                        input[pos.x + pos.y * stride]
                      : (float2) (0.f, 0.f);
            }

        }
    }
}


Complex complexMul(Complex v1, Complex v2)
{
    Complex result;

    result.s0 = v1.s0 * v2.s0 - v1.s1 * v2.s1;
    result.s1 = v1.s0 * v2.s1 + v2.s0 * v1.s1;

    return result;
}


Complex complexConj(Complex v1)
{
    Complex result;
    result.s0 = v1.s0;
    result.s1 = -v1.s1;
    return result;
}


ComplexPair interpDiagonallySymmetricULLR(const __local Complex* centreY, 
                                      size_t stride,
                                      const Complex coefficients[4])
{
    ComplexPair result;

    // Upper left quadrant
    result.a = complexMul(*(centreY - stride - 1), coefficients[0])
             + complexMul(*(centreY - stride), coefficients[1])
             + complexMul(*(centreY - 1), coefficients[2])
             + complexMul(*centreY, coefficients[3]);

    // Lower right
    result.b = complexMul(*(centreY + stride + 1), complexConj(coefficients[0]))
             + complexMul(*(centreY + stride), complexConj(coefficients[1]))
             + complexMul(*(centreY + 1), complexConj(coefficients[2]))
             + complexMul(*centreY, complexConj(coefficients[3]));

    return result;
}


ComplexPair interpDiagonallySymmetricURLL(const __local Complex* centreY, 
                                          size_t stride,
                                          const Complex coefficients[4])
{
    ComplexPair result;

    // Upper right
    result.a = complexMul(*(centreY - stride), coefficients[0])
             + complexMul(*(centreY - stride + 1), coefficients[1])
             + complexMul(*centreY, coefficients[2])
             + complexMul(*(centreY + 1), coefficients[3]);

    // Lower left
    result.b = complexMul(*(centreY + stride - 1), complexConj(coefficients[1]))
             + complexMul(*(centreY + stride), complexConj(coefficients[0]))
             + complexMul(*(centreY - 1), complexConj(coefficients[3]))
             + complexMul(*centreY, complexConj(coefficients[2]));

    return result;
}



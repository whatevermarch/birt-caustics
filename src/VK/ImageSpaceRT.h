const float SSRT_EPS = 1e-4;

// random function
// ref : https://thebookofshaders.com/10/
float randFromCoord(vec2 st) {
    return fract(sin(dot(st, vec2(12.9898, 78.233))) * 43758.5453123);
}

#define USE_NEW_TRACE

// Exact, unpolarized Fresnel equation
float fresnelUnpolarized(
    float cosI, // cosIncident
    float cosT, // cosTransmitted
    float n1, // refractiveIndexIncident,
    float n2 // refractiveIndexTransmitted
)
{
    float rs = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
    float rp = (n1 * cosT - n2 * cosI) / (n1 * cosT + n2 * cosI);
    return 0.5f * (rs * rs + rp * rp);
}
#ifndef USE_NEW_TRACE
//  trace rays through a given view
//  NOTE : users need to define these functions prior to this function call
//  - float fetchGBufDepth(vec2 coord, int mipLevel)
//  - float fetchRSMDepth(vec2 coord, int mipLevel)
//  - ivec2 getGBufDepthSize(int mipLevel)
//  - ivec2 getRSMDepthSize(int mipLevel)
bool traceOnView(
    vec3 origin, 
    vec3 direction, 
    TransformParams viewParams, 
    bool bCamera,
    float tMax,
    float depthBias,
    out float lastT, 
    out vec2 lastCoord)
{
    lastT = 0;
    lastCoord = vec2(-1);

    //  remember the view and clip space is RH (negative Z)
    //  determine projected trace direction
    const vec4 viewOri = viewParams.view * vec4(origin, 1.0f);
    const vec4 viewDir = viewParams.view * vec4(direction, 0.0f);

    //  handle special cases : too narrow direction vector
    //  this is to prevent an overflow of t value also
    const float near_linear = viewParams.nearPlane;
    const float far_linear = viewParams.farPlane;

    float tMax_actual = tMax;

    if (near_linear > (-viewOri.z) || far_linear < (-viewOri.z))
        return false;

    const vec2 projVec_xy = vec2(viewParams.invTanHalfFovH, viewParams.invTanHalfFovV);
    const vec2 projOri = viewOri.xy * projVec_xy / (-viewOri.z);
    vec2 moveDir;
    {
        vec3 viewDst = viewOri.xyz + viewDir.xyz * tMax_actual;

        //  pre-bound for near-far plane
        if (viewDst.z > -near_linear)
        {
            float t_atNear = (-near_linear - viewOri.z) / viewDir.z;
            viewDst = viewOri.xyz + viewDir.xyz * t_atNear;
            tMax_actual = t_atNear;
        }
        else if (viewDst.z < -far_linear)
        {
            float t_atFar = (-far_linear - viewOri.z) / viewDir.z;
            viewDst = viewOri.xyz + viewDir.xyz * t_atFar;
            tMax_actual = t_atFar;
        }

        vec2 projDst = viewDst.xy * projVec_xy / (-viewDst.z);
        moveDir = projDst - projOri;
    }

    //  initialize ray info
    lastCoord = vec2(0.5f) + vec2(projOri.x, -projOri.y) * 0.5f;

    const float lastProjSampleDepth = bCamera ? fetchGBufDepth(lastCoord, 0) : fetchRSMDepth(lastCoord, 0);
    const float lastViewSampleDepth = toViewDepth(lastProjSampleDepth, near_linear, far_linear);

    if (abs(viewOri.z - lastViewSampleDepth) <= depthBias)
        return true;
    else if (viewOri.z < lastViewSampleDepth)
        return false;

    //  calculate unit move direction in image space
    if (length(moveDir / 2) < SSRT_EPS)
    {
        if (viewDir.z < 0)
        {
            lastT = (lastViewSampleDepth - viewOri.z) / viewDir.z;
            return (lastProjSampleDepth < 1.0f) ? true : false; // hit
        }
        else
        {
            lastT = (-near_linear - viewOri.z) / viewDir.z;
            return false; // miss
        }
    }
    const vec2 unitMoveDir = normalize(vec2(moveDir.x, -moveDir.y));

    //  traverse for the first occlusion first
    bool bHit = false;
    int traverseLevel = 0;
    int maxTraverseLevel = 0; //bCamera ? textureQueryLevels(u_gbufDepth1N) : textureQueryLevels(u_rsmDepth1N);
    while (traverseLevel >= 0)
    {
        const vec2 nextBasis = vec2(1) / (bCamera ? getGBufDepthSize(traverseLevel) : getRSMDepthSize(traverseLevel));
        const vec2 nextCoord = lastCoord + unitMoveDir * (nextBasis.x < nextBasis.y ? nextBasis.x : nextBasis.y);

        if (nextCoord.x >= 0 && nextCoord.x <= 1 &&
            nextCoord.y >= 0 && nextCoord.y <= 1)
        {
            //  fetch scene's depth value
            const float nextProjSamplePos_z = bCamera ? fetchGBufDepth(nextCoord, traverseLevel) : fetchRSMDepth(nextCoord, traverseLevel); // = depth_nonlinear;
            const float nextViewSamplePos_z = toViewDepth(nextProjSamplePos_z, near_linear, far_linear); // = depth_linear;

            const vec2 nextProjSamplePos_xy = vec2(2, -2) * (nextCoord - 0.5f);

            //  calculate t if xy-coord (view space) is equal
            const float t_eqXY = abs(viewDir.x) > abs(viewDir.y) ?
                (-nextProjSamplePos_xy.x * viewOri.z - projVec_xy.x * viewOri.x) /
                (projVec_xy.x * viewDir.x + nextProjSamplePos_xy.x * viewDir.z) :
                (-nextProjSamplePos_xy.y * viewOri.z - projVec_xy.y * viewOri.y) /
                (projVec_xy.y * viewDir.y + nextProjSamplePos_xy.y * viewDir.z);

            if (t_eqXY <= tMax_actual)
            {
                const float nextViewRayEndPos_z = viewOri.z + viewDir.z * t_eqXY;

                //  check hit (remember that view depth (linear) is negative !!)
                if (nextViewRayEndPos_z > nextViewSamplePos_z + SSRT_EPS) // ray is still above the surface
                {
                    if (traverseLevel < maxTraverseLevel)
                        traverseLevel += 1;

                    lastT = t_eqXY;
                    lastCoord = nextCoord;
                    continue;
                }
                else if (traverseLevel == 0) // ray is hit or occluded
                {
                    if (nextViewRayEndPos_z >= nextViewSamplePos_z)
                    {
                        lastT = t_eqXY;
                        lastCoord = nextCoord;
                        bHit = true;
                    }
                    else
                    {
                        //  perform linear backtracking to fing an exact hit point
                        const float prevProjSamplePos_z = bCamera ? fetchGBufDepth(lastCoord, traverseLevel) : fetchRSMDepth(lastCoord, traverseLevel); // = depth_nonlinear;
                        const float prevViewSamplePos_z = toViewDepth(prevProjSamplePos_z, near_linear, far_linear); // = depth_linear;

                        const float prevViewRayEndPos_z = viewOri.z + viewDir.z * lastT;

                        const float dzdtSample = nextViewSamplePos_z - prevViewSamplePos_z;
                        const float dzdtRay = nextViewRayEndPos_z - prevViewRayEndPos_z;

                        if (abs(dzdtSample - dzdtRay) >= SSRT_EPS)
                        {
                            const float a_intersect = (prevViewSamplePos_z - prevViewRayEndPos_z) / (dzdtRay - dzdtSample);
                            if (0 <= a_intersect && a_intersect <= 1)
                            {
                                const float t_intersect = mix(lastT, t_eqXY, a_intersect);
                                lastT = t_intersect;
                                const vec2 coord_intersect = mix(lastCoord, nextCoord, a_intersect);
                                lastCoord = coord_intersect;

                                bHit = true;
                            }
                        }
                    }
                }
            }
            else if (traverseLevel == 0) // in case of exceeding t, use tMax as the last stop
            {
                lastT = tMax_actual;

                const vec3 lastViewRayEndPos = viewOri.xyz + viewDir.xyz * tMax_actual;
                const vec2 lastProjRayEndPos = lastViewRayEndPos.xy * projVec_xy / (-lastViewRayEndPos.z);
                lastCoord = vec2(0.5f) + vec2(lastProjRayEndPos.x, -lastProjRayEndPos.y) * 0.5f;
            }
        }
        else if (traverseLevel == 0)
        {
            vec2 lastT_atEdge = vec2(tMax_actual);
            if (nextCoord.x < 0) // exceed -1
                lastT_atEdge.x = (viewOri.z - projVec_xy.x * viewOri.x) / (viewDir.x * projVec_xy.x - viewDir.z);
            else if (nextCoord.x > 1) // exceed 1
                lastT_atEdge.x = (-viewOri.z - projVec_xy.x * viewOri.x) / (viewDir.x * projVec_xy.x + viewDir.z);
            if (nextCoord.y < 0) // exceed 1
                lastT_atEdge.y = (-viewOri.z - projVec_xy.y * viewOri.y) / (viewDir.y * projVec_xy.y + viewDir.z);
            else if (nextCoord.y > 1) // exceed -1
                lastT_atEdge.y = (viewOri.z - projVec_xy.y * viewOri.y) / (viewDir.y * projVec_xy.y - viewDir.z);

            if (lastT_atEdge.x < lastT_atEdge.y) // end at horizontal edge
            {
                lastT = lastT_atEdge.x;

                float bound = (unitMoveDir.x < 0) ? 0.0f : 1.0f;
                lastCoord.y = lastCoord.y + unitMoveDir.y * (bound - lastCoord.x) / unitMoveDir.x;
                lastCoord.x = bound;
            }
            else // end at vertical edge
            {
                lastT = lastT_atEdge.y;

                float bound = (unitMoveDir.y < 0) ? 0.0f : 1.0f;
                lastCoord.x = lastCoord.x + unitMoveDir.x * (bound - lastCoord.y) / unitMoveDir.y;
                lastCoord.y = bound;
            }

            bHit = false;
        }

        traverseLevel -= 1;
        maxTraverseLevel = traverseLevel;
    }

    return bHit;
}
#else
bool traceOnView(
    vec3 worldOri, 
    vec3 worldDir, 
    TransformParams viewParams,
    bool bCamera,
    float tMax,
    out float lastT,
    out vec2 lastCoord)
{
    lastT = 0;
    lastCoord = vec2(-1);

    const vec4 viewOri = viewParams.view * vec4(worldOri, 1.0f);
    const vec4 viewDir = viewParams.view * vec4(worldDir, 0.0f);

    const float invH = viewParams.invTanHalfFovH;
    const float invV = viewParams.invTanHalfFovV;
    const float nearZ = viewParams.nearPlane;
    const float farZ = viewParams.farPlane;

    //  remember the view and clip space is RH (negative Z)
    if (nearZ > (-viewOri.z) || farZ < (-viewOri.z))
        return false;

    const float fRange = farZ / (nearZ - farZ);
    const mat4 proj = mat4(
        invH,            0, 0, 0, // col1
		0, invV,            0, 0, // col2
		0, 0, fRange         ,-1, // col3
		0, 0, nearZ * fRange , 0  // col4
	);

    //  determine moving direction in screen space
    vec4 viewDst = viewOri + viewDir * tMax;
    
    //  pre-bound for near-far plane
    //  NOTE: this step is needed because it will prevent reverse projection-space XY
    if (viewDst.z > -nearZ)
    {
        float t_atNear = (-nearZ - viewOri.z) / viewDir.z;
        viewDst = viewOri + viewDir * t_atNear;
        tMax = t_atNear;
    }
    else if (viewDst.z < -farZ)
    {
        float t_atFar = (-farZ - viewOri.z) / viewDir.z;
        viewDst = viewOri + viewDir * t_atFar;
        tMax = t_atFar;
    }

    const vec4 projOri = (proj * viewOri) / (-viewOri.z/* = projOri.w */);
    const vec4 projDst = (proj * viewDst) / (-viewDst.z/* = projDst.w */);
    const vec2 moveDir = projDst.xy - projOri.xy;

    //  setup initial values before tracing
    lastCoord = vec2(0.5f) + vec2(projOri.x, -projOri.y) * 0.5f;
    const vec2 lastBasis = vec2(1) / (bCamera ? getGBufDepthSize(0) : getRSMDepthSize(0));
    float lastSampleDepth = bCamera ? fetchGBufDepth(lastCoord, 0) : fetchRSMDepth(lastCoord, 0);

    if (abs(viewOri.z - toViewDepth(lastSampleDepth, nearZ, farZ)) <= 0.015f)
        return true;

    //  if it's already occluded, don't proceed
    if (lastSampleDepth < projOri.z)
        return false;

    //  or if its direction is perpendicular to the screen, don't trace and calculate 't' directly!
    else if (length(moveDir / 2.0f) < (lastBasis.x < lastBasis.y ? lastBasis.x : lastBasis.y))
    {
        if (viewDir.z < 0)  // towards farZ plane
        {
            if (lastSampleDepth >= 1.0f)
                return false;
            
            lastT = (toViewDepth(lastSampleDepth, nearZ, farZ) - viewOri.z) / viewDir.z;
            if (lastT > tMax) 
            {
                lastT = tMax;
                return false;
            }
                
            return true;
        }
        else                // towards nearZ plane
        {
            lastT = (-nearZ - viewOri.z) / viewDir.z;
            return false;
        }
    }

    //  determine a direction in texture-space
    const vec2 unitMoveDir = normalize(vec2(moveDir.x, -moveDir.y));

    //  traverse for the first occlusion
    bool bHit = false;
    vec2 startCoord = lastCoord;
    int traverseLevel = 0;
    int maxTraverseLevel = 0; //bCamera ? textureQueryLevels(u_gbufDepth1N) : textureQueryLevels(u_rsmDepth1N); // ToDo : need fix!
    while (traverseLevel >= 0)
    {
        const vec2 nextBasis = vec2(1) / (bCamera ? getGBufDepthSize(traverseLevel) : getRSMDepthSize(traverseLevel));
        const vec2 nextCoord = startCoord + unitMoveDir * (nextBasis.x < nextBasis.y ? nextBasis.x : nextBasis.y);

        if(all(greaterThanEqual(nextCoord, vec2(0))) && 
            all(lessThanEqual(nextCoord, vec2(1))))
        {
            const vec2 nextProjPos2D = vec2(2, -2) * (nextCoord - 0.5f);
            const float nextT = abs(viewDir.x) > abs(viewDir.y) ?
                -dot(vec2(viewOri.x, viewOri.z), vec2(invH, nextProjPos2D.x)) / 
                    dot(vec2(viewDir.x, viewDir.z), vec2(invH, nextProjPos2D.x)) :
                -dot(vec2(viewOri.y, viewOri.z), vec2(invV, nextProjPos2D.y)) / 
                    dot(vec2(viewDir.y, viewDir.z), vec2(invV, nextProjPos2D.y));
            const vec4 nextViewPos = viewOri + viewDir * nextT;

            if(nextT <= tMax)
            {
                const float nextRayEndDepth = ((proj * nextViewPos) / (-nextViewPos.z/* = nextProjPos.w */)).z;
                const float nextSampleDepth = bCamera ? fetchGBufDepth(nextCoord, traverseLevel) : fetchRSMDepth(nextCoord, traverseLevel);
                
                if (nextRayEndDepth < nextSampleDepth) // still above
                {
                    if (traverseLevel < maxTraverseLevel)
                        traverseLevel += 1;
                    else
                        startCoord = nextCoord;

                    lastT = nextT;
                    lastCoord = nextCoord;

                    continue;
                }
                else if(traverseLevel == 0) // occluded, and it's over
                {
                    //  perform linear intersection to find an exact hit point (represented by 't' value)
                    lastSampleDepth = bCamera ? fetchGBufDepth(lastCoord, 0) : fetchRSMDepth(lastCoord, 0);
                    const float lastSampleZ = toViewDepth(lastSampleDepth, nearZ, farZ);
                    const float lastRayEndZ = viewOri.z + viewDir.z * lastT;

                    const float nextSampleZ = toViewDepth(nextSampleDepth, nearZ, farZ);
                    const float nextRayEndZ = nextViewPos.z;

                    const float dzdtSample = nextSampleZ - lastSampleZ;
                    const float dzdtRay = nextRayEndZ - lastRayEndZ;

                    if (abs(dzdtSample - dzdtRay) >= SSRT_EPS)
                    {
                        const float a_intersect = (lastSampleZ - lastRayEndZ) / (dzdtRay - dzdtSample);
                        if (0 <= a_intersect && a_intersect <= 1)
                        {
                            const float t_intersect = mix(lastT, nextT, a_intersect);
                            if (t_intersect <= tMax)
                            {
                                const vec2 coord_intersect = mix(lastCoord, nextCoord, a_intersect);

                                lastT = t_intersect;
                                bHit = true;
                            }
                        }
                    }
                }
            }
        }

        // occluded, but let's try at lower level
        startCoord = lastCoord;
        traverseLevel -= 1;
        maxTraverseLevel = traverseLevel;
    }

    return bHit;
}
#endif
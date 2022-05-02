float normalDistanceCos(vec3 n1, vec3 n2, float power)
{
	return pow(clamp(dot(n1,n2), 0, 1), power);
	//return 1.0f; // the original sample implement this. I don't know why???
}

float computeEdgeStoppingWeight(
    float ctrDepth, float pDepth, float phiDepth,
    vec3 ctrNormal, vec3 pNormal, float normPower, 
    float ctrLuminance, float pLuminance, float phiLuminance)
{
    const float wZ = abs(ctrDepth - pDepth) / phiDepth;
    const float wNormal = normalDistanceCos(ctrNormal, pNormal, normPower);
	const float wLuminance = abs(ctrLuminance - pLuminance) / phiLuminance;

	const float w = exp(- wZ - wLuminance) * wNormal;

	return w;
}
// From http://www.geeks3d.com/20141201/how-to-rotate-a-vertex-by-a-quaternion-in-glsl/
vec4 quat_mult(vec4 q1, vec4 q2)
{
	vec4 qr;
	qr.x = (q1.w * q2.x) + (q1.x * q2.w) + (q1.y * q2.z) - (q1.z * q2.y);
	qr.y = (q1.w * q2.y) - (q1.x * q2.z) + (q1.y * q2.w) + (q1.z * q2.x);
	qr.z = (q1.w * q2.z) + (q1.x * q2.y) - (q1.y * q2.x) + (q1.z * q2.w);
	qr.w = (q1.w * q2.w) - (q1.x * q2.x) - (q1.y * q2.y) - (q1.z * q2.z);
	return qr;
}

vec3 quat_rot_vec(vec4 quat, vec3 v)
{
	// From Wikipedia, s' = qs(q^-1)
	// https://en.wikipedia.org/wiki/Quaternions_and_spatial_rotation
	return quat_mult(
		quat_mult(quat, vec4(v, 0.0)),
		vec4(-quat.xyz, quat.w)
	).xyz;
}

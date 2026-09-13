#version 140

const vec3 ZERO = vec3(0.0, 0.0, 0.0);
//BBS: add grey and orange
//const vec3 GREY = vec3(0.9, 0.9, 0.9);
const vec3 ORANGE = vec3(0.8, 0.4, 0.0);
const vec3 LightRed = vec3(0.78, 0.0, 0.0);
const vec3 LightBlue = vec3(0.73, 1.0, 1.0);
const float EPSILON = 0.0001;

struct PrintVolumeDetection
{
	// 0 = rectangle, 1 = circle, 2 = custom, 3 = invalid
	int type;
    // type = 0 (rectangle):
    // x = min.x, y = min.y, z = max.x, w = max.y
    // type = 1 (circle):
    // x = center.x, y = center.y, z = radius
	vec4 xy_data;
    // x = min z, y = max z
	vec2 z_data;
};

struct SlopeDetection
{
    bool actived;
	float normal_z;
    mat3 volume_world_normal_matrix;
};

uniform vec4 uniform_color;
uniform bool use_color_clip_plane;
uniform vec4 uniform_color_clip_plane_1;
uniform vec4 uniform_color_clip_plane_2;
uniform SlopeDetection slope;

// Curved cut sheet (cut gizmo, Surface = Curved). When active the two halves are
// split by a height field z = f(u,v) sampled over the cut plane, not by the flat
// plane itself: curved_sheet_matrix takes a world point into the cut plane's own
// frame, (x,y) are mapped onto [0,1]^2 over +-curved_sheet_half_size and the
// texture holds f in mm along the plane normal. The comparison has to happen per
// FRAGMENT - a per-vertex dot would only bend as finely as the mesh is
// tessellated, which is exactly the flat-looking preview this replaces.
uniform bool  curved_sheet_active;
uniform sampler2D curved_sheet_tex;
uniform mat4  curved_sheet_matrix;
// Phase 2: the sheet's domain is a RECTANGLE fitted to the cut's cross-section,
// so u and v have their own half extent.
uniform vec2  curved_sheet_half_size;
// 0.0: the texture holds f in mm. Otherwise it holds (f/range + 1)/2, which is
// what the GL 2.1 fallback has to do - GL_LUMINANCE is fixed point on [0,1].
uniform float curved_sheet_range;
// DRAWN cut field (cut gizmo, Surface = Draw; 2026-09-12). A ruled strip swept along a
// drawn stroke is NOT a height field over the plane, so curved_sheet_tex above cannot
// describe it - there is no single-valued (u,v) -> z. What there is instead is the
// question the split itself asks: IS THE POINT INSIDE THE CUTTER SOLID. That is baked
// into a 3D SIGN FIELD over the part's bounding box in the cut plane's frame, NEGATIVE
// on the upper half, which is the same convention color_clip_plane already uses.
//
// draw_field_matrix takes a world point into the plane frame; origin and size normalise
// it into the texture's [0,1]^3. The fetch is NEAREST-filtered on the C++ side, because
// interpolating a two-valued sign puts a ragged, camera-dependent band across the
// boundary.
uniform bool      draw_field_active;
uniform sampler3D draw_field_tex;
uniform mat4      draw_field_matrix;
uniform vec3      draw_field_origin;
uniform vec3      draw_field_size;

// Phase 2, side visibility. Per-half alpha for the two colour-clip sides, so a
// half can be shown solid (1.0), ghosted (~0.25) or hidden. A NEGATIVE value
// means hidden: the fragment is discarded outright, which is the only way to see
// through a half rather than through it dimly. Defaults to 1.0/1.0 so every
// other caller of the colour clip - the flat cut, and any future user of it -
// is untouched. (GLSL uniforms cannot carry an initialiser, so the 1.0 default
// is set on the C++ side, in GLVolumeCollection::render.)
uniform float color_clip_side_alpha_1;
uniform float color_clip_side_alpha_2;

//BBS: add outline_color
uniform bool is_outline;
uniform sampler2D depth_tex;
uniform vec2 screen_size;

#ifdef ENABLE_ENVIRONMENT_MAP
    uniform sampler2D environment_tex;
    uniform bool use_environment_tex;
#endif // ENABLE_ENVIRONMENT_MAP

uniform PrintVolumeDetection print_volume;

uniform float z_far;
uniform float z_near;

in vec3 clipping_planes_dots;
in float color_clip_plane_dot;

// x = diffuse, y = specular;
in vec2 intensity;

in vec4 world_pos;
in float world_normal_z;
in vec3 eye_normal;

vec3 getBackfaceColor(vec3 fill) {
    float brightness = 0.2126 * fill.r + 0.7152 * fill.g + 0.0722 * fill.b;
    return (brightness > 0.75) ? vec3(0.11, 0.165, 0.208) : vec3(0.988, 0.988, 0.988);
}

// Silhouette edge detection & rendering algorithem by leoneruggiero
// https://www.shadertoy.com/view/DslXz2
#define INFLATE 1

float GetTolerance(float d, float k)
{
    // -------------------------------------------
    // Find a tolerance for depth that is constant
    // in view space (k in view space).
    //
    // tol = k*ddx(ZtoDepth(z))
    // -------------------------------------------
    
    float A=-   (z_far+z_near)/(z_far-z_near);
    float B=-2.0*z_far*z_near /(z_far-z_near);
    
    d = d*2.0-1.0;
    
    return -k*(d+A)*(d+A)/B;   
}

float DetectSilho(vec2 fragCoord, vec2 dir)
{
    // -------------------------------------------
    //   x0 ___ x1----o 
    //          :\    : 
    //       r0 : \   : r1
    //          :  \  : 
    //          o---x2 ___ x3
    //
    // r0 and r1 are the differences between actual
    // and expected (as if x0..3 where on the same
    // plane) depth values.
    // -------------------------------------------
    
    float x0 = abs(texture(depth_tex, (fragCoord + dir*-2.0) / screen_size).r);
    float x1 = abs(texture(depth_tex, (fragCoord + dir*-1.0) / screen_size).r);
    float x2 = abs(texture(depth_tex, (fragCoord + dir* 0.0) / screen_size).r);
    float x3 = abs(texture(depth_tex, (fragCoord + dir* 1.0) / screen_size).r);
    
    float d0 = (x1-x0);
    float d1 = (x2-x3);
    
    float r0 = x1 + d0 - x2;
    float r1 = x2 + d1 - x1;
    
    float tol = GetTolerance(x2, 0.04);
    
    return smoothstep(0.0, tol*tol, max( - r0*r1, 0.0));

}

float DetectSilho(vec2 fragCoord)
{
    return max(
        DetectSilho(fragCoord, vec2(1,0)), // Horizontal
        DetectSilho(fragCoord, vec2(0,1))  // Vertical
        );
}

out vec4 out_color;

void main()
{
    if (any(lessThan(clipping_planes_dots, ZERO)))
        discard;

    vec4 color;
	if (use_color_clip_plane) {
		float side = color_clip_plane_dot;
		if (draw_field_active) {
			// The DRAWN field wins over the sheet: a cut is Curved or Draw, never both,
			// and giving the newer one precedence means a stale sheet texture left over
			// from a mode switch cannot colour a drawn cut.
			vec3 local = (draw_field_matrix * vec4(world_pos.xyz, 1.0)).xyz;
			vec3 uvw = (local - draw_field_origin) / draw_field_size;
			// Outside the field the part does not exist, so clamping continues the
			// border value rather than snapping back to the flat plane at the box edge.
			float v = texture(draw_field_tex, clamp(uvw, 0.0, 1.0)).r;
			// The GL 2.1 fallback stores 0 for upper and 1 for lower (GL_LUMINANCE is
			// fixed point on [0,1] and cannot hold -1), so the test is against the
			// midpoint and works for both encodings: -1 and 0 are both below 0.5.
			side = v - 0.5;
		}
		else if (curved_sheet_active) {
			vec3 local = (curved_sheet_matrix * vec4(world_pos.xyz, 1.0)).xyz;
			vec2 uv = local.xy / (2.0 * curved_sheet_half_size) + vec2(0.5, 0.5);
			// Outside the sheet's domain the height field is not defined; clamp
			// so the split continues along the border value rather than snapping
			// back to the flat plane at the sheet's edge.
			float h = texture(curved_sheet_tex, clamp(uv, 0.0, 1.0)).r;
			if (curved_sheet_range > 0.0)
				h = (2.0 * h - 1.0) * curved_sheet_range;
			// color_clip_plane points along -normal (see GLVolumeCollection::
			// set_color_clip_plane), so keep the same sign convention here.
			side = h - local.z;
		}
		bool first = side < 0.0;
		// Side visibility. Hidden is a discard, not an alpha of zero: a
		// zero-alpha fragment still writes depth and would keep hiding whatever
		// is behind it, which is the whole point of hiding the near half.
		float side_alpha = first ? color_clip_side_alpha_1 : color_clip_side_alpha_2;
		if (side_alpha < 0.0)
			discard;
		color.rgb = first ? uniform_color_clip_plane_1.rgb : uniform_color_clip_plane_2.rgb;
		color.a = uniform_color.a * side_alpha;
    }
    else
	    color = uniform_color;

    if (slope.actived) {
         if(world_pos.z<0.1&&world_pos.z>-0.1)
         {
                color.rgb = LightBlue;
                color.a = 0.8;
         }
         else if( world_normal_z < slope.normal_z - EPSILON)
         {
                color.rgb = color.rgb * 0.5 + LightRed * 0.5;
                color.a = 0.8;
         }
    }
    // if the fragment is outside the print volume -> use darker color
	vec3 pv_check_min = ZERO;
	vec3 pv_check_max = ZERO;
    if (print_volume.type == 0) {
		// rectangle
		pv_check_min = world_pos.xyz - vec3(print_volume.xy_data.x, print_volume.xy_data.y, print_volume.z_data.x);
		pv_check_max = world_pos.xyz - vec3(print_volume.xy_data.z, print_volume.xy_data.w, print_volume.z_data.y);
	}
	else if (print_volume.type == 1) {
		// circle
		float delta_radius = print_volume.xy_data.z - distance(world_pos.xy, print_volume.xy_data.xy);
		pv_check_min = vec3(delta_radius, 0.0, world_pos.z - print_volume.z_data.x);
		pv_check_max = vec3(0.0, 0.0, world_pos.z - print_volume.z_data.y);
	}
	color.rgb = (any(lessThan(pv_check_min, ZERO)) || any(greaterThan(pv_check_max, ZERO))) ? mix(color.rgb, ZERO, 0.3333) : color.rgb;

    //BBS: add outline_color
    if (is_outline) {
        color = vec4(vec3(intensity.y) + color.rgb * intensity.x, color.a);
        vec2 fragCoord = gl_FragCoord.xy;
        float s = DetectSilho(fragCoord);
        // Makes silhouettes thicker.
        for(int i=1;i<=INFLATE; i++)
        {
           s = max(s, DetectSilho(fragCoord.xy + vec2(i, 0)));
           s = max(s, DetectSilho(fragCoord.xy + vec2(0, i)));
        }   
        out_color = vec4(mix(color.rgb, getBackfaceColor(color.rgb), s), color.a);
    }
#ifdef ENABLE_ENVIRONMENT_MAP
    else if (use_environment_tex)
        out_color = vec4(0.45 * texture(environment_tex, normalize(eye_normal).xy * 0.5 + 0.5).xyz + 0.8 * color.rgb * intensity.x, color.a);
#endif
    else
        out_color = vec4(vec3(intensity.y) + color.rgb * intensity.x, color.a);
}
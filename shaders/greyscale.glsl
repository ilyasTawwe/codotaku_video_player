//!DESC Greyscale test
//!HOOK NATIVE
//!BIND NATIVE
//!SAVE NATIVE
//!WIDTH NATIVE.w
//!HEIGHT NATIVE.h
vec4 hook() {
    vec4 c = NATIVE_tex(NATIVE_pos);
    return vec4(c.r, 0.5, 0.5, c.a);
}

#version 330

in INTERFACE {
	vec4 color;
	vec2 uv;
	float id;
} In;

uniform sampler2DArray lookParticles;
uniform bool useImages = false;

out vec4 fragColor;


void main(){
	fragColor = In.color;
	float alpha = texture(lookParticles, vec3(In.uv, In.id)).r;
	if(useImages){
		// Custom images are displayed as they are.
		fragColor.a *= alpha;
		return;
	}
	// Default look: a leaf, pointing up in the sprite.
	vec2 p = 2.0 * In.uv - 1.0;
	float edge = max(1.5 * fwidth(p.x), 0.02);
	// Blade: a pointed oval from the top of the stem to the tip, wider below its middle.
	float along = (p.y + 0.8) / 1.8;
	float halfWidth = 0.55 * sin(3.14159265359 * pow(clamp(along, 0.0, 1.0), 0.8));
	float blade = smoothstep(0.0, edge, halfWidth - abs(p.x)) * step(0.0, along) * step(along, 1.0);
	// Short stem below the blade.
	float stem = smoothstep(0.0, edge, 0.06 - abs(p.x)) * step(p.y, -0.8);
	// Darker vein along the middle, and one half slightly darker than the other.
	float shade = 1.0 - 0.3 * (1.0 - smoothstep(0.03, 0.03 + edge, abs(p.x)));
	shade *= p.x > 0.0 ? 1.0 : 0.82;
	fragColor.rgb *= mix(0.7, shade, blade);
	fragColor.a *= alpha * max(blade, stem);
}

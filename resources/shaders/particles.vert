#version 330
#define SETS_COUNT 12

layout(location = 0) in vec2 v;

uniform float time;
uniform float scale;
uniform vec3 baseColor[SETS_COUNT];
uniform vec2 inverseScreenSize;
uniform float pixelScale = 1.0; // rendered pixels per pixel of the final image

uniform int globalId;
uniform float duration; // seconds the leaves of a note last, from the note until they have vanished
uniform int channel;

uniform int texCount;
uniform float colorScale;

uniform float keyboardHeight = 0.25;

// Floating controls.
uniform float floatHeight = 1.0; // how high the leaves rise
uniform float sway = 1.0;        // how far they swing from side to side
uniform float seed = 0.0;        // changes with each note so repeated notes differ

uniform int minNote;
uniform float notesCount;
uniform bool horizontalMode = false;

vec2 flipIfNeeded(vec2 inPos){
	return horizontalMode ? vec2(inPos.y, -inPos.x) : inPos;
}

const float shifts[128] = float[](
0,0.5,1,1.5,2,3,3.5,4,4.5,5,5.5,6,7,7.5,8,8.5,9,10,10.5,11,11.5,12,12.5,13,14,14.5,15,15.5,16,17,17.5,18,18.5,19,19.5,20,21,21.5,22,22.5,23,24,24.5,25,25.5,26,26.5,27,28,28.5,29,29.5,30,31,31.5,32,32.5,33,33.5,34,35,35.5,36,36.5,37,38,38.5,39,39.5,40,40.5,41,42,42.5,43,43.5,44,45,45.5,46,46.5,47,47.5,48,49,49.5,50,50.5,51,52,52.5,53,53.5,54,54.5,55,56,56.5,57,57.5,58,59,59.5,60,60.5,61,61.5,62,63,63.5,64,64.5,65,66,66.5,67,67.5,68,68.5,69,70,70.5,71,71.5,72,73,73.5,74
);

out INTERFACE {
	vec4 color;
	vec2 uv;
	float id;
} Out;


float rand(vec2 co){
	return fract(sin(dot(co.xy ,vec2(12.9898,78.233))) * 43758.5453);
}


void main(){
	Out.id = float(gl_InstanceID % texCount);
	Out.uv = v + 0.5;

	// Per-particle random values, different for each note.
	float particleId = float(gl_InstanceID);
	float noteSeed = seed + 1.37 * float(globalId);
	float randBorn = rand(vec2(particleId + 23.9, noteSeed + 6.7));
	float randLife = rand(vec2(particleId + 43.1, noteSeed + 9.4));
	float randRise = rand(vec2(particleId + 31.7, noteSeed + 7.1));
	float randSway = rand(vec2(particleId + 57.3, noteSeed + 3.7));
	float randPhase = rand(vec2(particleId + 11.3, noteSeed + 5.3));
	float randDrift = rand(vec2(particleId + 77.9, noteSeed + 2.9));
	float randSpin = rand(vec2(particleId + 5.1, noteSeed + 4.4));

	// Seconds since the note. The scene gives the animation a 0.25s head start,
	// removed here so the leaves start from the key.
	float seconds = max(time * duration - 0.25, 0.0);
	// The leaves leave the key one after another, and each has its own lifespan,
	// so they thin out progressively instead of vanishing all at once.
	float born = 0.35 * duration * randBorn;
	float age = max(seconds - born, 0.0);
	float lifespan = (duration - born) * (0.7 + 0.3 * randLife);
	float progress = clamp(age / lifespan, 0.0, 1.0);

	// Small per-leaf brightness variation.
	float bright = 0.75 + 0.4 * rand(vec2(particleId, 12.1));
	// Appear softly, then fade out over the last third of the lifespan.
	float alpha = smoothstep(0.0, 0.2, age) * (1.0 - smoothstep(0.65, 1.0, progress));
	Out.color = vec4(colorScale * baseColor[channel] * bright, alpha);

	// Horizontal shift is based on the note ID.
	float xshift = -1.0 + ((shifts[globalId] - shifts[int(minNote)]) * 2.0 + 1.0) / notesCount;
	vec2 globalShift = vec2(xshift, (2.0 * keyboardHeight - 1.0)-0.02);

	float sizeVar = 0.7 + 0.6 * rand(vec2(particleId, 9.3));
	float spriteSize = 0.012 * scale * sizeVar;
	// A sprite smaller than a pixel of the final image would flicker or vanish: draw it
	// at a minimal size and make it fainter instead, so that small sizes read as fine dust.
	float spritePixels = 0.5 * spriteSize / inverseScreenSize.x;
	float grow = max(1.0, 2.0 * pixelScale / max(spritePixels, 0.0001));
	Out.color.a *= max(1.0 / grow, 0.3);

	// Each leaf swings from side to side at its own rate.
	float swing = 6.28318530718 * ((0.25 + 0.3 * randSway) * age + randPhase);
	// It leans with its swing while slowly spinning, and its width is squashed
	// back and forth to suggest that it flutters.
	float angle = 6.28318530718 * randSpin + 0.6 * cos(swing) + 1.2 * (randSpin - 0.5) * age;
	float flutter = 0.45 + 0.55 * abs(cos(0.8 * swing + 6.28318530718 * randDrift));
	vec2 corner = vec2(flutter * v.x, v.y);
	corner = vec2(cos(angle) * corner.x - sin(angle) * corner.y, sin(angle) * corner.x + cos(angle) * corner.y);
	vec2 vertexShift = grow * spriteSize * corner;

	float screenRatio = inverseScreenSize.y/inverseScreenSize.x;
	vec2 screenScaling = vec2(1.0, horizontalMode ? (1.0/screenRatio) : screenRatio);

	// Float: rise out of the key, easing off with age, each leaf at its own pace.
	float rise = 0.4 * floatHeight * (0.5 + 0.5 * randRise) * (1.0 - pow(1.0 - progress, 1.8));
	// Start from slightly different spots across the key, then swing from side to side
	// more and more widely while drifting a little to one side.
	float side = 0.6 * (randDrift - 0.5) / notesCount;
	side += sway * smoothstep(0.0, 0.35, progress) * (0.5 + 0.5 * randSway) * 0.035 * sin(swing);
	side += sway * 0.1 * (randPhase - 0.5) * progress;

	vec2 particlePos = globalShift + screenScaling * vec2(side, rise);
	vec2 finalPos = particlePos + screenScaling * vertexShift;
	// Output final particle position.
	gl_Position = vec4(flipIfNeeded(finalPos), 0.0, 1.0);
}

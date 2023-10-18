#include "AepelzensParasites.hpp"
#include "stmlib/utils/random.h"
#include "warps/dsp/modulator.h"
#include <array>

struct Tapeworm : Module {
	enum ParamIds {
		ALGORITHM_PARAM,
		TIMBRE_PARAM,
		STATE_PARAM,
		LEVEL1_PARAM,
		LEVEL2_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		LEVEL1_INPUT,
		LEVEL2_INPUT,
		ALGORITHM_INPUT,
		TIMBRE_INPUT,
		CARRIER_INPUT,
		MODULATOR_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		MODULATOR_OUTPUT,
		AUX_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		CARRIER_GREEN_LIGHT, CARRIER_RED_LIGHT,
		ALGORITHM_LIGHT,
		NUM_LIGHTS = ALGORITHM_LIGHT + 3
	};

	int frame = 0;
	warps::Modulator modulator {};
	warps::ShortFrame inputFrames[60] {};
	warps::ShortFrame outputFrames[60] {};
	dsp::SchmittTrigger stateTrigger;

	//Parasites variables
	std::array<stmlib::OnePole, 4> filter_;

	warps::Parameters parameters_;
	warps::Parameters previous_parameters_;

	warps::FloatFrame feedback_sample {};
	int32_t write_head = 0;
	float write_position = 0.0f;
	std::array<warps::FloatFrame, 3> previous_samples {};

	float lp_time = 0.0f;
	float lp_rate;

	/* everything that follows will be used as delay buffer */
	warps::ShortFrame delay_buffer_[8192 + 4096];
	std::array<float, warps::kMaxBlockSize> internal_modulation_;

	std::array<std::array<float, warps::kMaxBlockSize>, 3> buffer_;
	std::array<std::array<float, warps::kMaxBlockSize * warps::kOversampling>, 2> src_buffer_;

	float feedback_sample_;

	enum DelaySize {
		DELAY_SIZE = (sizeof(delay_buffer_)
          + sizeof(internal_modulation_)
          + sizeof(buffer_)
          + sizeof(src_buffer_)
          + sizeof(feedback_sample_)) / sizeof(warps::ShortFrame) - 4
	};

	enum DelayInterpolation {
		INTERPOLATION_ZOH,
		INTERPOLATION_LINEAR,
		INTERPOLATION_HERMITE,
	};

	DelayInterpolation delay_interpolation_;

	// Taken from eurorack\warps\ui.cc
	const uint8_t algorithm_palette[10][3] = {
		{ 0, 192, 64 },
		{ 64, 255, 0 },
		{ 255, 255, 0 },
		{ 255, 64, 0 },
		{ 255, 0, 0 },
		{ 255, 0, 64 },
		{ 255, 0, 255 },
		{ 0, 0, 255 },
		{ 0, 255, 192 },
		{ 0, 255, 192 },
	};

	Tapeworm() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);		
		configParam(Tapeworm::ALGORITHM_PARAM, 0.0, 8.0, 0.0, "Algorithm");
		configParam(Tapeworm::TIMBRE_PARAM, 0.0, 1.0, 0.5, "Timbre");
		configButton(Tapeworm::STATE_PARAM, "Internal oscillator mode");
		// configSwitch(Tapeworm::STATE_PARAM, 0.0, 1.0, 0.0, "Internal oscillator mode", {
		// 	"External", "Sine / Sawtooth", "Triangle / Pulse", "Sawtooth / LP filtered noise"
		// });
		configParam(Tapeworm::LEVEL1_PARAM, 0.0, 1.0, 1.0, "External oscillator amplitude / internal oscillator frequency");
		configParam(Tapeworm::LEVEL2_PARAM, 0.0, 1.0, 1.0, "Modulator amplitude");
		
		configInput(LEVEL1_INPUT, "Level 1");
		configInput(LEVEL2_INPUT, "Level 2");
		configInput(ALGORITHM_INPUT, "Algorithm");
		configInput(TIMBRE_INPUT, "Timbre");
		configInput(CARRIER_INPUT, "Carrier");
		configInput(MODULATOR_INPUT, "Modulator");

		configOutput(MODULATOR_OUTPUT, "Modulator");
		configOutput(AUX_OUTPUT, "Auxiliary");

		configBypass(MODULATOR_INPUT, MODULATOR_OUTPUT);

		// modulator.Init(96000.0f);
		delay_interpolation_ = INTERPOLATION_HERMITE;
	}
	
	void process(const ProcessArgs& args) override;
	void ProcessDelay(warps::ShortFrame* input, warps::ShortFrame* output, size_t size);

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "shape", json_integer(parameters_.carrier_shape));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		if (json_t* shapeJ = json_object_get(rootJ, "shape")) {
			parameters_.carrier_shape = json_integer_value(shapeJ);
		}
	}

	void onReset() override { parameters_.carrier_shape = 0; }
	void onRandomize() override { parameters_.carrier_shape = random::u32() % 4; }
};

// Delay Code from Parasites firmware
void Tapeworm::ProcessDelay(warps::ShortFrame* input, warps::ShortFrame* output, size_t size) {

	using namespace warps;
	ShortFrame *buffer = delay_buffer_;

	// static FloatFrame feedback_sample;
	// static int32_t write_head = 0;
	// static float write_position = 0.0f;
	// static FloatFrame previous_samples[3] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};

	float time = previous_parameters_.modulation_parameter * (DELAY_SIZE - 10) + 5;
	float time_end = parameters_.modulation_parameter * (DELAY_SIZE - 10) + 5;
	float time_increment = (time_end - time) / static_cast<float>(size);

	float feedback = previous_parameters_.raw_level[0];
	float feedback_end = parameters_.raw_level[0];
	float feedback_increment = (feedback_end - feedback) / static_cast<float>(size);

	float drywet = previous_parameters_.raw_level[1];
	float drywet_end = parameters_.raw_level[1];
	float drywet_increment = (drywet_end - drywet) / static_cast<float>(size);

	float rate = previous_parameters_.raw_algorithm;
	rate = rate * 2.0f - 1.0f;
	rate *= rate * rate;
	float rate_end = parameters_.raw_algorithm;
	rate_end = rate_end * 2.0f - 1.0f;
	rate_end = rate_end * rate_end * rate_end;
	float rate_increment = (rate_end - rate) / static_cast<float>(size);

	filter_[0].set_f<stmlib::FREQUENCY_FAST>(0.0008f);
	filter_[1].set_f<stmlib::FREQUENCY_FAST>(0.0008f);

	while (size--) {

		//static float lp_time = 0.0f;
		ONE_POLE(lp_time, time, 0.00002f);

		//static float lp_rate;
		ONE_POLE(lp_rate, rate, 0.007f);
		float sample_rate = fabsf(lp_rate);
		CONSTRAIN(sample_rate, 0.001f, 1.0f);
		int direction = lp_rate > 0.0f ? 1 : -1;

		FloatFrame in;
		in.l = static_cast<float>(input->l) / 32768.0f;
		in.r = static_cast<float>(input->r) / 32768.0f;

		FloatFrame fb;

		if (parameters_.carrier_shape == 3) {
			// invert feedback channels (ping-pong)
			fb.l = feedback_sample.r * feedback * 1.1f;
			fb.r = feedback_sample.l * feedback * 1.1f;
		} else if (parameters_.carrier_shape == 2) {
			// simulate tape hiss with a bit of noise
			float noise1 = stmlib::Random::GetFloat();
			float noise2 = stmlib::Random::GetFloat();
			fb.l = feedback_sample.l + noise1 * 0.002f;
			fb.r = feedback_sample.r + noise2 * 0.002f;
			// apply filters: fixed high-pass and varying low-pass with attenuation
			filter_[2].set_f<stmlib::FREQUENCY_FAST>(feedback / 12.0f);
			filter_[3].set_f<stmlib::FREQUENCY_FAST>(feedback / 12.0f);
			fb.l = filter_[0].Process<stmlib::FILTER_MODE_HIGH_PASS>(fb.l);
			fb.r = filter_[1].Process<stmlib::FILTER_MODE_HIGH_PASS>(fb.r);
			fb.l = feedback * (2.0f - feedback) * 1.1f *
			       filter_[2].Process<stmlib::FILTER_MODE_LOW_PASS>(fb.l);
			fb.r = feedback * (2.0f - feedback) * 1.1f *
			       filter_[3].Process<stmlib::FILTER_MODE_LOW_PASS>(fb.r);
			// apply soft saturation with a bit of bias
			fb.l = stmlib::SoftLimit(fb.l * 1.4f + 0.1f) / 1.4f - stmlib::SoftLimit(0.1f);
			fb.r = stmlib::SoftLimit(fb.r * 1.4f + 0.1f) / 1.4f - stmlib::SoftLimit(0.1f);
		} else if (parameters_.carrier_shape == 0) {
			// open feedback loop
			fb.l = feedback * 1.1f * in.r;
			fb.r = feedback_sample.l;
			in.r = 0.0f;
		} else {
			// classic dual delay
			fb.l = feedback_sample.l * feedback * 1.1f;
			fb.r = feedback_sample.r * feedback * 1.1f;
		}

		// input + feedback
		FloatFrame mix;
		mix.l = in.l + fb.l;
		mix.r = in.r + fb.r;

		// write to buffer
		while (write_position < 1.0f) {

			// read somewhere between the input and the previous input
			FloatFrame s = {0, 0};

			if (delay_interpolation_ == INTERPOLATION_ZOH) {
				s.l = mix.l;
				s.r = mix.r;
			} else if (delay_interpolation_ == INTERPOLATION_LINEAR) {
				s.l = previous_samples[0].l + (mix.l - previous_samples[0].l) * write_position;
				s.r = previous_samples[0].r + (mix.r - previous_samples[0].r) * write_position;
			} else if (delay_interpolation_ == INTERPOLATION_HERMITE) {
				FloatFrame xm1 = previous_samples[2];
				FloatFrame x0 = previous_samples[1];
				FloatFrame x1 = previous_samples[0];
				FloatFrame x2 = mix;

				FloatFrame c = { (x1.l - xm1.l) * 0.5f,
				                 (x1.r - xm1.r) * 0.5f
				               };
				FloatFrame v = { static_cast<float>((x0.l - x1.l)), static_cast<float>((x0.r - x1.r)) };
				FloatFrame w = { c.l + v.l, c.r + v.r };
				FloatFrame a = { w.l + v.l + (x2.l - x0.l) * 0.5f,
				                 w.r + v.r + (x2.r - x0.r) * 0.5f
				               };
				FloatFrame b_neg = { w.l + a.l, w.r + a.r };
				float t = write_position;
				s.l = ((((a.l * t) - b_neg.l) * t + c.l) * t + x0.l);
				s.r = ((((a.r * t) - b_neg.r) * t + c.r) * t + x0.r);
			}

			// write this to buffer
			buffer[write_head].l = stmlib::Clip16((s.l) * 32768.0f);
			buffer[write_head].r = stmlib::Clip16((s.r) * 32768.0f);

			write_position += 1.0f / sample_rate;

			write_head += direction;
			// wraparound
			if (write_head >= DELAY_SIZE)
				write_head -= DELAY_SIZE;
			else if (write_head < 0)
				write_head += DELAY_SIZE;
		}

		write_position--;

		previous_samples[2] = previous_samples[1];
		previous_samples[1] = previous_samples[0];
		previous_samples[0] = mix;

		// read from buffer

		float index = write_head - write_position * sample_rate * direction - lp_time;

		while (index < 0) {
			index += DELAY_SIZE;
		}

		MAKE_INTEGRAL_FRACTIONAL(index);

		ShortFrame xm1 = buffer[index_integral];
		ShortFrame x0 = buffer[(index_integral + 1) % DELAY_SIZE];
		ShortFrame x1 = buffer[(index_integral + 2) % DELAY_SIZE];
		ShortFrame x2 = buffer[(index_integral + 3) % DELAY_SIZE];

		FloatFrame wet;

		if (delay_interpolation_ == INTERPOLATION_ZOH) {
			wet.l = xm1.l;
			wet.r = xm1.r;
		} else if (delay_interpolation_ == INTERPOLATION_LINEAR) {
			wet.l = xm1.l + (x0.l - xm1.l) * index_fractional;
			wet.r = xm1.r + (x0.r - xm1.r) * index_fractional;
		} else if (delay_interpolation_ == INTERPOLATION_HERMITE) {
			FloatFrame c = { (x1.l - xm1.l) * 0.5f,
			                 (x1.r - xm1.r) * 0.5f
			               };
			FloatFrame v = { static_cast<float>((x0.l - x1.l)), static_cast<float>((x0.r - x1.r)) };
			FloatFrame w = { c.l + v.l, c.r + v.r };
			FloatFrame a = { w.l + v.l + (x2.l - x0.l) * 0.5f,
			                 w.r + v.r + (x2.r - x0.r) * 0.5f
			               };
			FloatFrame b_neg = { w.l + a.l, w.r + a.r };
			float t = index_fractional;
			wet.l = ((((a.l * t) - b_neg.l) * t + c.l) * t + x0.l);
			wet.r = ((((a.r * t) - b_neg.r) * t + c.r) * t + x0.r);
		}

		wet.l /= 32768.0f;
		wet.r /= 32768.0f;

		// attenuate output at low sample rate to mask stupid
		// discontinuity bug
		float gain = sample_rate / 0.01f;
		CONSTRAIN(gain, 0.0f, 1.0f);
		wet.l *= gain * gain;
		wet.r *= gain * gain;

		feedback_sample = wet;

		float fade_in = stmlib::Interpolate(lut_xfade_in, drywet, 256.0f);
		float fade_out = stmlib::Interpolate(lut_xfade_out, drywet, 256.0f);

		if (parameters_.carrier_shape == 0) {
			// if open feedback loop, AUX is the wet signal and OUT
			// crossfades between inputs
			in.r = static_cast<float>(input->r) / 32768.0f;
			output->l = stmlib::Clip16((fade_out * in.l + fade_in * in.r) * 32768.0f);
			output->r = stmlib::Clip16(wet.r * 32768.0f);
		} else if (parameters_.carrier_shape == 2) {
			// analog mode -> soft-clipping
			output->l = stmlib::SoftConvert((fade_out * in.l + fade_in * wet.l) * 2.0f);
			output->r = stmlib::SoftConvert((fade_out * in.r + fade_in * wet.r) * 2.0f);
		} else {
			output->l = stmlib::Clip16((fade_out * in.l + fade_in * wet.l) * 32768.0f);
			output->r = stmlib::Clip16((fade_out * in.r + fade_in * wet.r) * 32768.0f);
		}

		feedback += feedback_increment;
		rate += rate_increment;
		time += time_increment;
		drywet += drywet_increment;
		input++;
		output++;
	}

	previous_parameters_ = parameters_;
}


void Tapeworm::process(const ProcessArgs& args) {
	// State trigger
	warps::Parameters* p = &parameters_;
	if (stateTrigger.process(params[STATE_PARAM].getValue())) {
		p->carrier_shape = (p->carrier_shape + 1) % 4;
	}
	lights[CARRIER_GREEN_LIGHT].setBrightness((p->carrier_shape == 1 || p->carrier_shape == 2) ? 1.0 : 0.0);
	lights[CARRIER_RED_LIGHT].setBrightness((p->carrier_shape == 2 || p->carrier_shape == 3) ? 1.0 : 0.0);

	// Buffer loop
	if (++frame >= 60) {
		frame = 0;

		p->modulation_algorithm = clamp(params[ALGORITHM_PARAM].getValue() / 8.0f + inputs[ALGORITHM_INPUT].getVoltage() / 5.0f, 0.0f, 1.0f);
		p->raw_level[0] = clamp(params[LEVEL1_PARAM].getValue(), 0.0f, 1.0f);
		p->raw_level[1] = clamp(params[LEVEL2_PARAM].getValue(), 0.0f, 1.0f);

		if (inputs[LEVEL1_INPUT].isConnected())
			p->raw_level[0] *= clamp(inputs[LEVEL1_INPUT].getVoltage() / 5.0f, 0.0f, 1.0f);
		if (inputs[LEVEL2_INPUT].isConnected())
			p->raw_level[1] *= clamp(inputs[LEVEL2_INPUT].getVoltage() / 5.0f, 0.0f, 1.0f);

		//p->raw_algorithm_pot = clampf(params[ALGORITHM_PARAM].getValue() /8.0, 0.0, 1.0);
		// float val = clampf(params[ALGORITHM_PARAM].getValue() /8.0, 0.0, 1.0);
		// val = stmlib::Interpolate(warps::lut_pot_curve, val, 512.0f);
		// p->raw_algorithm_pot = val;

		//p->raw_algorithm_cv = clampf(inputs[ALGORITHM_INPUT].getVoltage() /5.0, -1.0,1.0);
		//According to the cv-scaler this does not seem to use the plot curve
		p->raw_algorithm = clamp(params[ALGORITHM_PARAM].getValue() / 8.0f + inputs[ALGORITHM_INPUT].getVoltage() / 5.0f, 0.0f, 1.0f);
		{
			// Taken from eurorack\warps\ui.cc
			float zone = 8.0f * p->modulation_algorithm;
            MAKE_INTEGRAL_FRACTIONAL(zone);
            int zone_fractional_i = static_cast<int>(zone_fractional * 256.0f);
            for (int i = 0; i < 3; i++) {
                int a = algorithm_palette[zone_integral][i];
                int b = algorithm_palette[zone_integral + 1][i];
                lights[ALGORITHM_LIGHT + i].setBrightness(static_cast<float>(a + ((b - a) * zone_fractional_i >> 8)) / 255.0f);
            }
		}

		p->modulation_parameter = clamp(params[TIMBRE_PARAM].getValue() + inputs[TIMBRE_INPUT].getVoltage() / 5.0f, 0.0f, 1.0f);
		ProcessDelay(inputFrames, outputFrames, 60);
	}

	inputFrames[frame].l = clamp(static_cast<int>((inputs[CARRIER_INPUT].getVoltage() / 16.0 * 0x8000)), -0x8000, 0x7fff);
	inputFrames[frame].r = clamp(static_cast<int>((inputs[MODULATOR_INPUT].getVoltage() / 16.0 * 0x8000)), -0x8000, 0x7fff);
	outputs[MODULATOR_OUTPUT].setVoltage(static_cast<float>(outputFrames[frame].l) / 0x8000 * 5.0);
	outputs[AUX_OUTPUT].setVoltage(static_cast<float>(outputFrames[frame].r) / 0x8000 * 5.0);
}


struct TapewormWidget : ModuleWidget {
	TapewormWidget(Tapeworm* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Tapeworm.svg")));

		// addChild(createWidget<ScrewSilver>(Vec(15, 0)));
		// addChild(createWidget<ScrewSilver>(Vec(120, 0)));
		// addChild(createWidget<ScrewSilver>(Vec(15, 365)));
		// addChild(createWidget<ScrewSilver>(Vec(120, 365)));

		addParam(createParamCentered<Rogan6PSWhite>(Vec(73.556641, 96.560532), module, Tapeworm::ALGORITHM_PARAM));
		addParam(createParam<Rogan1PSWhite>(Vec(94, 173), module, Tapeworm::TIMBRE_PARAM));
		addParam(createParam<TL1105>(Vec(16, 182), module, Tapeworm::STATE_PARAM));
		addParam(createParam<Trimpot>(Vec(14, 213), module, Tapeworm::LEVEL1_PARAM));
		addParam(createParam<Trimpot>(Vec(53, 213), module, Tapeworm::LEVEL2_PARAM));

		addInput(createInput<PJ301MPort>(Vec(8, 273), module, Tapeworm::LEVEL1_INPUT));
		addInput(createInput<PJ301MPort>(Vec(44, 273), module, Tapeworm::LEVEL2_INPUT));
		addInput(createInput<PJ301MPort>(Vec(80, 273), module, Tapeworm::ALGORITHM_INPUT));
		addInput(createInput<PJ301MPort>(Vec(116, 273), module, Tapeworm::TIMBRE_INPUT));
		addInput(createInput<PJ301MPort>(Vec(8, 316), module, Tapeworm::CARRIER_INPUT));
		addInput(createInput<PJ301MPort>(Vec(44, 316), module, Tapeworm::MODULATOR_INPUT));

		addOutput(createOutput<PJ301MPort>(Vec(80, 316), module, Tapeworm::MODULATOR_OUTPUT));
		addOutput(createOutput<PJ301MPort>(Vec(116, 316), module, Tapeworm::AUX_OUTPUT));

		addChild(createLight<SmallLight<GreenRedLight>>(Vec(21, 169), module, Tapeworm::CARRIER_GREEN_LIGHT));
		addChild(createLightCentered<Rogan6PSLight<RedGreenBlueLight>>(Vec(73.556641, 96.560532), module, Tapeworm::ALGORITHM_LIGHT));
	}
};

Model *modelTapeworm = createModel<Tapeworm, TapewormWidget>("Tapeworm");

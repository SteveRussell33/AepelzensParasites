#include "AepelzensParasites.hpp"
#include "warps/dsp/modulator.h"
#include "warps/resources.h"

#pragma GCC diagnostic ignored "-Wclass-memaccess"

struct Warps : Module {
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
	warps::Modulator modulator;
	warps::ShortFrame inputFrames[60] {};
	warps::ShortFrame outputFrames[60] {};
	dsp::SchmittTrigger stateTrigger;

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

	Warps() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(Warps::ALGORITHM_PARAM, 0.0, 8.0, 0.0, "Algorithm");
		configParam(Warps::TIMBRE_PARAM, 0.0, 1.0, 0.5, "Timbre");
		configParam(Warps::STATE_PARAM, 0.0, 1.0, 0.0, "Internal oscillator mode");
		configParam(Warps::LEVEL1_PARAM, 0.0, 1.0, 1.0, "External oscillator amplitude / internal oscillator frequency");
		configParam(Warps::LEVEL2_PARAM, 0.0, 1.0, 1.0, "Modulator amplitude");

		memset(&modulator, 0, sizeof(modulator));
		modulator.Init(96000.0f);
	}
	
	void process(const ProcessArgs& args) override;

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		warps::Parameters* p = modulator.mutable_parameters();
		json_object_set_new(rootJ, "shape", json_integer(p->carrier_shape));
		json_object_set_new(rootJ, "mode", json_integer(modulator.feature_mode()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		warps::Parameters* p = modulator.mutable_parameters();
		if (json_t* shapeJ = json_object_get(rootJ, "shape")) {
			p->carrier_shape = json_integer_value(shapeJ);
		}
		if (json_t* modeJ = json_object_get(rootJ, "mode")) {
		  	modulator.set_feature_mode((warps::FeatureMode)json_integer_value(modeJ));
		}
	}

	void onReset() override {
		warps::Parameters* p = modulator.mutable_parameters();
		p->carrier_shape = 0;
		modulator.set_feature_mode(warps::FEATURE_MODE_META);
	}

	void onRandomize() override {
		warps::Parameters* p = modulator.mutable_parameters();
		p->carrier_shape = random::u32() % 4;
	}
};

void Warps::process(const ProcessArgs& args) {
	// State trigger
	warps::Parameters* p = modulator.mutable_parameters();
	if (stateTrigger.process(params[STATE_PARAM].getValue())) {
		p->carrier_shape = (p->carrier_shape + 1) % 4;
	}
	lights[CARRIER_GREEN_LIGHT].setBrightness((p->carrier_shape == 1 || p->carrier_shape == 2) ? 1.0 : 0.0);
	lights[CARRIER_RED_LIGHT].setBrightness((p->carrier_shape == 2 || p->carrier_shape == 3) ? 1.0 : 0.0);

	// Buffer loop
	if (++frame >= 60) {
		frame = 0;

		// Normal Warps' level inputs to 5v and make pots attenuate to match hardware and manual
		// https://github.com/VCVRack/AudibleInstruments/pull/107
		p->channel_drive[0] = clamp(params[LEVEL1_PARAM].getValue() * inputs[LEVEL1_INPUT].getNormalVoltage(5.0f) / 5.0f, 0.0f, 1.0f);
		p->channel_drive[1] = clamp(params[LEVEL2_PARAM].getValue() * inputs[LEVEL2_INPUT].getNormalVoltage(5.0f) / 5.0f, 0.0f, 1.0f);

		p->modulation_algorithm = clamp(params[ALGORITHM_PARAM].getValue() / 8.0 + inputs[ALGORITHM_INPUT].getVoltage() / 5.0f, 0.0f, 1.0f);
		p->raw_level[0] = clamp(params[LEVEL1_PARAM].getValue(), 0.0f, 1.0f);
		p->raw_level[1] = clamp(params[LEVEL2_PARAM].getValue(), 0.0f, 1.0f);

		//p->raw_algorithm_pot = clampf(params[ALGORITHM_PARAM].getValue() /8.0, 0.0, 1.0);
		float val = clamp(params[ALGORITHM_PARAM].getValue() /8.0f, 0.0f, 1.0f);
		val = stmlib::Interpolate(warps::lut_pot_curve, val, 512.0f);
		p->raw_algorithm_pot = val;

		p->raw_algorithm_cv = clamp(inputs[ALGORITHM_INPUT].getVoltage() /5.0f, -1.0f,1.0f);
		//According to the cv-scaler this does not seem to use the plot curve
		p->raw_algorithm = clamp(params[ALGORITHM_PARAM].getValue() /8.0f + inputs[ALGORITHM_INPUT].getVoltage() /5.0f, 0.0f, 1.0f);
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

		// p->frequency_shift_pot = params[ALGORITHM_PARAM].getValue() / 8.0;
		// p->frequency_shift_cv = clampf(inputs[ALGORITHM_INPUT].getVoltage() / 5.0, -1.0, 1.0);
		// p->phase_shift = p->modulation_algorithm;

		// level 1 pot still operates additively with level 1 cv for controlling the frequency of the internal oscillator
		p->note = 60.0 * params[LEVEL1_PARAM].getValue() + 12.0 * inputs[LEVEL1_INPUT].getNormalVoltage(2.0) + 12.0;
		p->note += log2f(96000.0 / args.sampleRate) * 12.0;

		modulator.Process(inputFrames, outputFrames, 60);
	}

	inputFrames[frame].l = clamp((int) (inputs[CARRIER_INPUT].getVoltage() / 16.0 * 0x8000), -0x8000, 0x7fff);
	inputFrames[frame].r = clamp((int) (inputs[MODULATOR_INPUT].getVoltage() / 16.0 * 0x8000), -0x8000, 0x7fff);
	outputs[MODULATOR_OUTPUT].setVoltage((float)outputFrames[frame].l / 0x8000 * 5.0);
	outputs[AUX_OUTPUT].setVoltage((float)outputFrames[frame].r / 0x8000 * 5.0);
}


struct WarpsWidget : ModuleWidget {
	WarpsWidget(Warps* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Wasp.svg")));

		// addChild(createWidget<ScrewSilver>(Vec(15, 0)));
		// addChild(createWidget<ScrewSilver>(Vec(120, 0)));
		// addChild(createWidget<ScrewSilver>(Vec(15, 365)));
		// addChild(createWidget<ScrewSilver>(Vec(120, 365)));

		addParam(createParamCentered<Rogan6PSWhite>(Vec(73.556641, 96.560532), module, Warps::ALGORITHM_PARAM));
		addParam(createParam<Rogan1PSWhite>(Vec(94, 173), module, Warps::TIMBRE_PARAM));
		addParam(createParam<TL1105>(Vec(16, 182), module, Warps::STATE_PARAM));
		addParam(createParam<Trimpot>(Vec(14, 213), module, Warps::LEVEL1_PARAM));
		addParam(createParam<Trimpot>(Vec(53, 213), module, Warps::LEVEL2_PARAM));

		addInput(createInput<PJ301MPort>(Vec(8, 273), module, Warps::LEVEL1_INPUT));
		addInput(createInput<PJ301MPort>(Vec(44, 273), module, Warps::LEVEL2_INPUT));
		addInput(createInput<PJ301MPort>(Vec(80, 273), module, Warps::ALGORITHM_INPUT));
		addInput(createInput<PJ301MPort>(Vec(116, 273), module, Warps::TIMBRE_INPUT));

		addInput(createInput<PJ301MPort>(Vec(8, 316), module, Warps::CARRIER_INPUT));
		addInput(createInput<PJ301MPort>(Vec(44, 316), module, Warps::MODULATOR_INPUT));
		addOutput(createOutput<PJ301MPort>(Vec(80, 316), module, Warps::MODULATOR_OUTPUT));
		addOutput(createOutput<PJ301MPort>(Vec(116, 316), module, Warps::AUX_OUTPUT));

		addChild(createLight<SmallLight<GreenRedLight>>(Vec(21, 169), module, Warps::CARRIER_GREEN_LIGHT));
		addChild(createLightCentered<Rogan6PSLight<RedGreenBlueLight>>(Vec(73.556641, 96.560532), module, Warps::ALGORITHM_LIGHT));
	}

	struct WarpsModeItem : MenuItem {
		Warps* module;
		warps::FeatureMode mode;
		void onAction(const event::Action &e) override {
			//module->playback = playback;
			module->modulator.set_feature_mode(mode);
		}
		void step() override {
			rightText = (module->modulator.feature_mode() == mode) ? "✔" : "";
			MenuItem::step();
		}
	};

	void appendContextMenu(Menu* menu) override {
		Warps* module = dynamic_cast<Warps*>(this->module);
		assert(module);

		menu->addChild(new MenuSeparator);
		menu->addChild(construct<MenuLabel>(&MenuLabel::text, "Mode"));

		menu->addChild(construct<WarpsModeItem>(&WarpsModeItem::text, "Meta", &WarpsModeItem::module, module, &WarpsModeItem::mode, warps::FEATURE_MODE_META));
		menu->addChild(construct<WarpsModeItem>(&WarpsModeItem::text, "Fold", &WarpsModeItem::module, module, &WarpsModeItem::mode, warps::FEATURE_MODE_FOLD));
		menu->addChild(construct<WarpsModeItem>(&WarpsModeItem::text, "Chebyschev", &WarpsModeItem::module, module, &WarpsModeItem::mode, warps::FEATURE_MODE_CHEBYSCHEV));
		menu->addChild(construct<WarpsModeItem>(&WarpsModeItem::text, "Frequency Shifter", &WarpsModeItem::module, module, &WarpsModeItem::mode, warps::FEATURE_MODE_FREQUENCY_SHIFTER));
		menu->addChild(construct<WarpsModeItem>(&WarpsModeItem::text, "Bitcrusher", &WarpsModeItem::module, module, &WarpsModeItem::mode, warps::FEATURE_MODE_BITCRUSHER));
		menu->addChild(construct<WarpsModeItem>(&WarpsModeItem::text, "Comparator", &WarpsModeItem::module, module, &WarpsModeItem::mode, warps::FEATURE_MODE_COMPARATOR));
		menu->addChild(construct<WarpsModeItem>(&WarpsModeItem::text, "Vocoder", &WarpsModeItem::module, module, &WarpsModeItem::mode, warps::FEATURE_MODE_VOCODER));
	// #ifdef DOPPLER_PANNER
		menu->addChild(construct<WarpsModeItem>(&WarpsModeItem::text, "Doppler", &WarpsModeItem::module, module, &WarpsModeItem::mode, warps::FEATURE_MODE_DOPPLER));
	// #endif
		//menu->addChild(construct<WarpsModeItem>(&WarpsModeItem::text, "Delay", &WarpsModeItem::module, module, &WarpsModeItem::mode, warps::FEATURE_MODE_DELAY));
	}
};

Model *modelWarps = createModel<Warps, WarpsWidget>("Warps");

#include "AepelzensParasites.hpp"
#include "tides/generator.h"
#include "tides/cv_scaler.h"

#pragma GCC diagnostic ignored "-Wclass-memaccess"

struct Tides : Module {
	enum ParamIds {
		MODE_PARAM,
		RANGE_PARAM,

		FREQUENCY_PARAM,
		FM_PARAM,

		SHAPE_PARAM,
		SLOPE_PARAM,
		SMOOTHNESS_PARAM,
		Q_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		SHAPE_INPUT,
		SLOPE_INPUT,
		SMOOTHNESS_INPUT,

		TRIG_INPUT,
		FREEZE_INPUT,
		PITCH_INPUT,
		FM_INPUT,
		LEVEL_INPUT,

		CLOCK_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		HIGH_OUTPUT,
		LOW_OUTPUT,
		UNI_OUTPUT,
		BI_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		MODE_GREEN_LIGHT, MODE_RED_LIGHT,
		PHASE_GREEN_LIGHT, PHASE_RED_LIGHT,
		RANGE_GREEN_LIGHT, RANGE_RED_LIGHT,
		ENUMS(Q_LIGHTS, 3),
		NUM_LIGHTS
	};

	const int16_t kOctave = 12 * 128;
	bool sheep;
	tides::Generator generator;
	uint8_t quantize = 0;
	int frame = 0;
	uint8_t lastGate;
	dsp::SchmittTrigger modeTrigger;
	dsp::SchmittTrigger rangeTrigger;
	
	Tides() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);		
		configButton(Tides::MODE_PARAM, "Output mode");
		configButton(Tides::RANGE_PARAM, "Frequency range");
		// configSwitch(Tides::MODE_PARAM, 0.0, 1.0, 0.0, "Output mode", {"AD", "Looping", "AR"});
		// configSwitch(Tides::RANGE_PARAM, 0.0, 1.0, 0.0, "Frequency range", {"High", "Medium", "Low"});
		configParam(Tides::FREQUENCY_PARAM, -48.0, 48.0, 0.0, "Main frequency");
		configParam(Tides::FM_PARAM, -12.0, 12.0, 0.0, "FM input attenuverter");
		configParam(Tides::SHAPE_PARAM, -1.0, 1.0, 0.0, "Shape");
		configParam(Tides::SLOPE_PARAM, -1.0, 1.0, 0.0, "Slope");
		configParam(Tides::SMOOTHNESS_PARAM, -1.0, 1.0, 0.0, "Smoothness");
		configParam(Tides::Q_PARAM, 0.0, 7.0, 0.0, "Quantizer scale")->snapEnabled = true;
		
		configInput(SHAPE_INPUT, "Shape");
		configInput(SLOPE_INPUT, "Slope");
		configInput(SMOOTHNESS_INPUT, "Smoothness");
		configInput(TRIG_INPUT, "Trigger");
		configInput(FREEZE_INPUT, "Freeze");
		configInput(PITCH_INPUT, "Pitch (1V/oct)");
		configInput(FM_INPUT, "FM");
		configInput(LEVEL_INPUT, "Level");
		configInput(CLOCK_INPUT, "Clock");

		configOutput(HIGH_OUTPUT, "High tide");
		configOutput(LOW_OUTPUT, "Low tide");
		configOutput(UNI_OUTPUT, "Unipolar");
		configOutput(BI_OUTPUT, "Bipolar");

		memset(&generator, 0, sizeof(generator));
		generator.Init();
		generator.set_sync(false);
		onReset();
	}
	
	void process(const ProcessArgs& args) override;

	void onReset() override {
		generator.set_range(tides::GENERATOR_RANGE_MEDIUM);
		generator.set_mode(tides::GENERATOR_MODE_LOOPING);
		sheep = false;
	}

	void onRandomize() override {
		generator.set_range(static_cast<tides::GeneratorRange>((random::u32() % 3)));
		generator.set_mode(static_cast<tides::GeneratorMode>((random::u32() % 3)));
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "mode", json_integer(static_cast<int>(generator.mode())));
		json_object_set_new(rootJ, "range", json_integer(static_cast<int>(generator.range())));
		json_object_set_new(rootJ, "sheep", json_boolean(sheep));
		json_object_set_new(rootJ, "featureMode", json_integer(static_cast<int>(generator.feature_mode_)));
		json_object_set_new(rootJ, "QuantizerMode", json_integer(quantize));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		if(json_t* featModeJ = json_object_get(rootJ, "featureMode")) {
		    generator.feature_mode_ = static_cast<tides::Generator::FeatureMode>(json_integer_value(featModeJ));
		}
		if (json_t* modeJ = json_object_get(rootJ, "mode")) {
			generator.set_mode(static_cast<tides::GeneratorMode>(json_integer_value(modeJ)));
		}
		if (json_t* rangeJ = json_object_get(rootJ, "range")) {
			generator.set_range(static_cast<tides::GeneratorRange>(json_integer_value(rangeJ)));
		}
		if (json_t* sheepJ = json_object_get(rootJ, "sheep")) {
			sheep = json_boolean_value(sheepJ);
		}
		if (json_t* quantizerJ = json_object_get(rootJ, "QuantizerMode")) {
			quantize = json_integer_value(quantizerJ);
		}
	}
};

void Tides::process(const ProcessArgs& args) {
	tides::GeneratorMode mode = generator.mode();
	if (modeTrigger.process(params[MODE_PARAM].getValue())) {
		mode = static_cast<tides::GeneratorMode>((static_cast<int>(mode + 1) % 3));
		generator.set_mode(mode);
	}
	lights[MODE_GREEN_LIGHT].setBrightness((mode == 1 || mode == 2) ? 1.0 : 0.0);
	lights[MODE_RED_LIGHT].setBrightness((mode == 0 || mode == 1) ? 1.0 : 0.0);

	tides::GeneratorRange range = generator.range();
	if (rangeTrigger.process(params[RANGE_PARAM].getValue())) {
		range = static_cast<tides::GeneratorRange>((static_cast<int>(range + 1) % 3));
		generator.set_range(range);
	}
	lights[RANGE_GREEN_LIGHT].setBrightness((range == 1 || range == 2) ? 1.0 : 0.0);
	lights[RANGE_RED_LIGHT].setBrightness((range == 0 || range == 1) ? 1.0 : 0.0);

	//Buffer loop
	if (generator.writable_block()) {
		// Pitch
		float pitchParam = clamp(params[FREQUENCY_PARAM].getValue() + inputs[PITCH_INPUT].getVoltage() * 12.0f, -60.0f, 60.0f);
		float fm = clamp(inputs[FM_INPUT].getVoltage() / 5.0f * params[FM_PARAM].getValue() / 12.0f, -1.0f, 1.0f) * 0x600;

		pitchParam += 60.0;
		// this is probably not original but seems useful to keep the same frequency as in normal mode
		if (generator.feature_mode_ == tides::Generator::FEAT_MODE_HARMONIC)
		    pitchParam -= 12;

		// this is equivalent to bitshifting by 7bits
		int16_t pitch = static_cast<int16_t>(pitchParam * 0x80);

		// Deviate from spec here: use trimpot and/or menu
		// to select scale instead of Mode & Range buttons 
		if ((quantize = params[Q_PARAM].getValue())) {
		// if (quantize) {
		    uint16_t semi = pitch >> 7;
		    uint16_t octaves = semi / 12 ;
		    semi -= octaves * 12;
		    // pitch = octaves * tides::kOctave + tides::quantize_lut[quantize - 1][semi];
		    pitch = octaves * kOctave + tides::quantize_lut[quantize - 1][semi];
		    // from ui.cc
			lights[Q_LIGHTS + 0].setBrightness((quantize & 1) ? 1.0 : 0.0);
			lights[Q_LIGHTS + 1].setBrightness((quantize & 2) ? 1.0 : 0.0);
			lights[Q_LIGHTS + 2].setBrightness((quantize & 4) ? 1.0 : 0.0);
		} else { // quantize is off
			for (size_t i = 0; i < 3; i++) lights[Q_LIGHTS + i].setBrightness(0.0);
		}

		// Scale to the global sample rate
		pitch += log2f(48000.0 / args.sampleRate) * 12.0 * 0x80;

		if (generator.feature_mode_ == tides::Generator::FEAT_MODE_HARMONIC) {
		    generator.set_pitch_high_range(clamp(pitch, -0x8000, 0x7fff), fm);
		}
		else {
		    generator.set_pitch(clamp(pitch, -0x8000, 0x7fff), fm);
		}

		if (generator.feature_mode_ == tides::Generator::FEAT_MODE_RANDOM) {
		    //TODO: should this be inverted?
		    generator.set_pulse_width(clamp(1.0 - params[FM_PARAM].getValue() / 12.0f, 0.0f, 2.0f) * 0x7fff);
		}

		// Slope, smoothness, pitch
		int16_t shape = clamp(params[SHAPE_PARAM].getValue() + inputs[SHAPE_INPUT].getVoltage() / 5.0f, -1.0f, 1.0f) * 0x7fff;
		int16_t slope = clamp(params[SLOPE_PARAM].getValue() + inputs[SLOPE_INPUT].getVoltage() / 5.0f, -1.0f, 1.0f) * 0x7fff;
		int16_t smoothness = clamp(params[SMOOTHNESS_PARAM].getValue() + inputs[SMOOTHNESS_INPUT].getVoltage() / 5.0f, -1.0f, 1.0f) * 0x7fff;
		generator.set_shape(shape);
		generator.set_slope(slope);
		generator.set_smoothness(smoothness);

		// Sync
		// Slight deviation from spec here.
		// Instead of toggling sync by holding the range button, just enable it if the clock port is plugged in.
		generator.set_sync(inputs[CLOCK_INPUT].isConnected());
		generator.FillBuffer();
#ifdef WAVETABLE_HACK
		generator.Process(sheep);
#endif
	}

	// Level
	uint16_t level = clamp(inputs[LEVEL_INPUT].getNormalVoltage(8.0) / 8.0f, 0.0f, 1.0f) * 0xffff;
	if (level < 32)
		level = 0;

	uint8_t gate = 0;
	if (inputs[FREEZE_INPUT].getVoltage() >= 0.7)
		gate |= tides::CONTROL_FREEZE;
	if (inputs[TRIG_INPUT].getVoltage() >= 0.7)
		gate |= tides::CONTROL_GATE;
	if (inputs[CLOCK_INPUT].getVoltage() >= 0.7)
		gate |= tides::CONTROL_CLOCK;
	if (!(lastGate & tides::CONTROL_CLOCK) && (gate & tides::CONTROL_CLOCK))
		gate |= tides::CONTROL_GATE_RISING;
	if (!(lastGate & tides::CONTROL_GATE) && (gate & tides::CONTROL_GATE))
		gate |= tides::CONTROL_GATE_RISING;
	if ((lastGate & tides::CONTROL_GATE) && !(gate & tides::CONTROL_GATE))
		gate |= tides::CONTROL_GATE_FALLING;
	lastGate = gate;

	const tides::GeneratorSample& sample = generator.Process(gate);

	uint32_t uni = sample.unipolar;
	int32_t bi = sample.bipolar;

	uni = uni * level >> 16;
	bi = -bi * level >> 16;
	float unif = static_cast<float>(uni) / 0xffff;
	float bif = static_cast<float>(bi) / 0x8000;

	outputs[HIGH_OUTPUT].setVoltage(sample.flags & tides::FLAG_END_OF_ATTACK ? 0.0 : 5.0);
	outputs[LOW_OUTPUT].setVoltage(sample.flags & tides::FLAG_END_OF_RELEASE ? 0.0 : 5.0);
	outputs[UNI_OUTPUT].setVoltage(unif * 8.0);
	outputs[BI_OUTPUT].setVoltage(bif * 5.0);

	if (sample.flags & tides::FLAG_END_OF_ATTACK)
		unif *= -1.0;
	lights[PHASE_GREEN_LIGHT].setSmoothBrightness(fmaxf(0.0, unif), args.sampleTime);
	lights[PHASE_RED_LIGHT].setSmoothBrightness(fmaxf(0.0, -unif), args.sampleTime);
}


struct TidesWidget : ModuleWidget {
	TidesWidget(Tides* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Cycles.svg")));

		// addChild(createWidget<ScrewSilver>(Vec(15, 0)));
		// addChild(createWidget<ScrewSilver>(Vec(180, 0)));
		// addChild(createWidget<ScrewSilver>(Vec(15, 365)));
		// addChild(createWidget<ScrewSilver>(Vec(180, 365)));

		addParam(createParam<CKD6>(Vec(20, 52), module, Tides::MODE_PARAM));
		addParam(createParam<CKD6>(Vec(20, 93), module, Tides::RANGE_PARAM));

		addParam(createParam<Rogan3PSGreen>(Vec(78, 60), module, Tides::FREQUENCY_PARAM));
		addParam(createParam<Rogan1PSGreen>(Vec(156, 66), module, Tides::FM_PARAM));

		addParam(createParam<Rogan1PSWhite>(Vec(13, 155), module, Tides::SHAPE_PARAM));
		addParam(createParam<Rogan1PSWhite>(Vec(85, 155), module, Tides::SLOPE_PARAM));
		addParam(createParam<Rogan1PSWhite>(Vec(156, 155), module, Tides::SMOOTHNESS_PARAM));
		addParam(createParam<Trimpot>(Vec(172, 35), module, Tides::Q_PARAM));

		addInput(createInput<PJ301MPort>(Vec(21, 219), module, Tides::SHAPE_INPUT));
		addInput(createInput<PJ301MPort>(Vec(93, 219), module, Tides::SLOPE_INPUT));
		addInput(createInput<PJ301MPort>(Vec(164, 219), module, Tides::SMOOTHNESS_INPUT));

		addInput(createInput<PJ301MPort>(Vec(21, 274), module, Tides::TRIG_INPUT));
		addInput(createInput<PJ301MPort>(Vec(57, 274), module, Tides::FREEZE_INPUT));
		addInput(createInput<PJ301MPort>(Vec(93, 274), module, Tides::PITCH_INPUT));
		addInput(createInput<PJ301MPort>(Vec(128, 274), module, Tides::FM_INPUT));
		addInput(createInput<PJ301MPort>(Vec(164, 274), module, Tides::LEVEL_INPUT));

		addInput(createInput<PJ301MPort>(Vec(21, 316), module, Tides::CLOCK_INPUT));
		addOutput(createOutput<PJ301MPort>(Vec(57, 316), module, Tides::HIGH_OUTPUT));
		addOutput(createOutput<PJ301MPort>(Vec(93, 316), module, Tides::LOW_OUTPUT));
		addOutput(createOutput<PJ301MPort>(Vec(128, 316), module, Tides::UNI_OUTPUT));
		addOutput(createOutput<PJ301MPort>(Vec(164, 316), module, Tides::BI_OUTPUT));

		addChild(createLight<MediumLight<GreenRedLight>>(Vec(57, 61), module, Tides::MODE_GREEN_LIGHT));
		addChild(createLight<MediumLight<GreenRedLight>>(Vec(57, 82), module, Tides::PHASE_GREEN_LIGHT));
		addChild(createLight<MediumLight<GreenRedLight>>(Vec(57, 102), module, Tides::RANGE_GREEN_LIGHT));
		for (size_t i = 0; i < 3; i++) {
			addChild(createLight<SmallLight<GreenLight>>(Vec(150 + (i * 7), 40), module, Tides::Q_LIGHTS + i));
		}
	}

	void appendContextMenu(Menu* menu) override {
		Tides* module = dynamic_cast<Tides*>(this->module);
		assert(module);

#ifdef WAVETABLE_HACK
		menu->addChild(new MenuSeparator);
		menu->addChild(createBoolPtrMenuItem("Sheep", "", &module->sheep));
#endif
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Mode"));

		struct ModeNameAndId {
			std::string name;
			tides::Generator::FeatureMode fmode;
		};
		static const std::vector<ModeNameAndId> modeLabels = {
			{"Original function generator", tides::Generator::FEAT_MODE_FUNCTION},
			{"Two Bumps - Harmonic osc", 	tides::Generator::FEAT_MODE_HARMONIC},
			{"Two Drunks - Random walk", 	tides::Generator::FEAT_MODE_RANDOM}
		};
		for (auto modeLabel : modeLabels) {
			menu->addChild(createCheckMenuItem(modeLabel.name, "",
				[=]() {return module->generator.feature_mode_ == modeLabel.fmode;},
				[=]() {module->generator.feature_mode_ = modeLabel.fmode;}
			));
		}

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Quantizer"));

		static const std::vector<std::string> quantizeLabels = {
			"Off", "Semitones", "Ionian", "Aeolian", "Whole tones", "Pentatonic Minor", "Pent-3", "Fifths"
		};
		for (size_t i = 0; i < quantizeLabels.size(); i++) {
			menu->addChild(createCheckMenuItem(quantizeLabels[i], "",
				[=]() {return module->quantize == i;},
				[=]() {module->quantize = i;}
			));
		}
	}
};

Model *modelTides = createModel<Tides, TidesWidget>("Tides");

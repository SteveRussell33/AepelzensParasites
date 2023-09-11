#include "rack.hpp"

using namespace rack;

extern Plugin *pluginInstance;

extern Model *modelWarps;
extern Model *modelTapeworm;
extern Model *modelTides;

template <typename Base>
struct Rogan6PSLight : Base {
	Rogan6PSLight() {
		this->box.size = mm2px(Vec(23.04, 23.04));
	}
};
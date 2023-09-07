#include "AepelzensParasites.hpp"


Plugin *pluginInstance;

void init(Plugin *p) {
	pluginInstance = p;

	p->addModel(modelWarps);
	p->addModel(modelTapeworm);
	p->addModel(modelTides);
}

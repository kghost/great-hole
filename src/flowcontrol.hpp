#ifndef FLOWCONTROL_H
#define FLOWCONTROL_H

template<typename flow_op>
class flow_control {
	public:
		flow_control(flow_op *o) : op(o), congesting(false), low_water_mark(100), high_water_mark(500) {}

		// XXX: implement red(random early detection)

		void after_read() {
			if (!congesting && op->size() > high_water_mark) {
				congesting = true;
				op->pause();
			}
		}

		void after_write() {
			if (congesting && op->size() < low_water_mark) {
				congesting = false;
				op->resume();
			}
		}

		int low_water_mark;
		int high_water_mark;

	private:
		bool congesting;
		flow_op *op;
};

#endif /* end of include guard: FLOWCONTROL_H */

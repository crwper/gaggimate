#ifndef PUMP_H
#define PUMP_H

class Pump {
  public:
    virtual ~Pump() = default;

    virtual void setup();
    virtual void loop();
    virtual void setPower(float setpoint);

    // Current commanded pump duty (0..100, percent). For closed-loop variants,
    // this is the value actually being applied to the actuator, not the user
    // setpoint. Used for `.slog` v6+ recording and any other introspection.
    virtual float getOutput() const = 0;
};

#endif // PUMP_H

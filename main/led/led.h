#ifndef _LED_H_
#define _LED_H_

class Led {
public:
    virtual ~Led() = default;
    // Set the led state based on the device state
    virtual void OnStateChanged() = 0;
    virtual void TurnOff() = 0;
};


class NoLed : public Led {
public:
    virtual void OnStateChanged() override {}
    virtual void TurnOff() override {}
};

#endif // _LED_H_

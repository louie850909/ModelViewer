#pragma once

class Helper {
public:
    static float CreateHaltonSequence(int index, int base) {
        float f = 1.0f;
        float r = 0.0f;
        int current = index;
        while (current > 0) {
            f = f / base;
            r = r + f * (current % base);
            current = current / base;
        }
        return r;
    }
};
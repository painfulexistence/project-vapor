#pragma once
#include <random>

namespace Vapor {

    class RNG {
    public:
        RNG() {
            _rng = std::mt19937(_r());
            _floatDist = std::uniform_real_distribution<float>(0.0f, 1.0f);
            _intDist = std::uniform_int_distribution<int>(0, 1);
        }
        ~RNG() {
        }

        float RandomFloat() {
            return _floatDist(_rng);
        }
        float RandomFloatInRange(float min, float max) {
            return min + (max - min) * RandomFloat();
        }
        int RandomInt() {
            return _intDist(_rng);
        }
        int RandomIntInRange(int min, int max) {
            _intDist.param(std::uniform_int_distribution<int>::param_type(min, max));
            int result = _intDist(_rng);
            _intDist.param(std::uniform_int_distribution<int>::param_type(0, 1));
            return result;
        }

    private:
        std::random_device _r;
        std::mt19937 _rng;
        std::uniform_real_distribution<float> _floatDist;
        std::uniform_int_distribution<int> _intDist;
    };

}// namespace Vapor

// Usage example
// RNG rng;
// float randomFloat = rng.RandomFloat();
// int randomInt = rng.RandomInt();
// float randomFloatInRange = rng.RandomFloatInRange(0.0f, 1.0f);
// int randomIntInRange = rng.RandomIntInRange(0, 100);
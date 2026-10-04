#pragma once

template <typename FeatureType> struct ContextData
{
    std::unique_ptr<FeatureType> feature;
    NVSDK_NGX_Parameter* createParams = nullptr;
    bool ownsCreateParams = false; // false when createParams is the game's own table (passthrough), never free it
    int changeBackendCounter = 0;
};

#pragma once

#include "ComponentVisualizer.h"

class FBox3DComponentVisualizer final : public FComponentVisualizer
{
public:
    virtual void DrawVisualization(const UActorComponent* Component, const FSceneView* View,
        FPrimitiveDrawInterface* PDI) override;
};

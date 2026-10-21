package com.nqind.keyboard67

data class RelayTouchInfo(
    val touchId: Int,
    val xPos: Float,
    val yPos: Float,
    val pressureValue: Float,
    val areaValue: Float,
    val timestampLong: Long,
    val jormyActionId: Int,
)

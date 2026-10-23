// PoC payload. The revision glyph is the human-visible half of verification:
// after flashing, the LED matrix must show "A1".
const REVISION = "A1"

bluetooth.startUartService()
basic.showString(REVISION)

basic.forever(function () {
    basic.showIcon(IconNames.Heart)
    basic.pause(400)
    basic.showIcon(IconNames.SmallHeart)
    basic.pause(400)
})

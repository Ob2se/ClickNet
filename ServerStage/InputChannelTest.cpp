#include "ClickNetInputChannel.h"
#include "InputTimeline.h"
#include <cassert>
#include <iostream>

int main()
{
    clicknet::InputSender sender;
    clicknet::InputReceiptWindow receiver;
    InputTimeline timeline;
    clicknet::wire::Input input;
    input.tick = 100; input.moveX = 1;
    sender.Queue(input); // First packet is lost.
    input.tick = 103; input.jump = true;
    sender.Queue(input); // Jump packet is also lost.
    sender.Attempted(100000);
    assert(!sender.RetryDue(149999) && sender.RetryDue(150000));
    input.tick = 106; input.jump = false; input.moveX = -1;
    sender.Queue(input);
    const auto history = sender.Packet();
    // Deliver newest first, then recover gaps. Receipt of newest must not ack gaps.
    assert(receiver.Accept(history.commands.back().sequence));
    timeline.Add(history.commands.back());
    sender.Acknowledge(receiver.sequence, receiver.bits);
    assert(sender.PendingThrough(103));
    for (const auto& command : sender.Packet().commands)
        if (receiver.Accept(command.sequence)) timeline.Add(command);
    assert(timeline.At(100).moveX == 1);
    assert(timeline.At(103).jump);
    assert(!timeline.At(104).jump);
    auto simultaneous = history.commands[1];
    simultaneous.jump = false;
    timeline.Add(simultaneous);
    assert(timeline.At(103).jump); // Rebased same-step movement preserves the event.
    assert(timeline.At(106).moveX == -1);
    // Lost acknowledgement causes retries, but no second jump or timeline mutation.
    for (const auto& command : history.commands) assert(!receiver.Accept(command.sequence));
    sender.Acknowledge(receiver.sequence, receiver.bits);
    assert(sender.Packet().commands.empty());
    assert(!sender.PendingThrough(106));
    assert(!sender.RetryDue(999999));

    auto bytes = clicknet::wire::Encode(history);
    clicknet::wire::InputPacket decoded;
    assert(clicknet::wire::Decode(bytes.data(), bytes.size(), decoded));
    assert(decoded.commands[1].jump && decoded.commands[2].sequence == 3);
    assert(!clicknet::wire::Decode(bytes.data(), bytes.size() - 1, decoded));
    bytes[4] = 17;
    assert(!clicknet::wire::Decode(bytes.data(), bytes.size(), decoded));
    auto invalid = history;
    invalid.commands[1].sequence = invalid.commands[0].sequence;
    bytes = clicknet::wire::Encode(invalid);
    assert(!clicknet::wire::Decode(bytes.data(), bytes.size(), decoded));
    invalid = history; invalid.commands[0].moveY = 2;
    bytes = clicknet::wire::Encode(invalid);
    assert(!clicknet::wire::Decode(bytes.data(), bytes.size(), decoded));
    // Bounded history never leaves the newest input behind a missing old packet.
    for (unsigned i = 0; i < 100; ++i) { input.tick = 200 + i; sender.Queue(input); }
    auto latest = sender.Packet();
    assert(latest.commands.size() == clicknet::wire::MaxInputsPerPacket);
    assert(latest.commands.back().tick == 299 && sender.Expired() == 36);
    assert(clicknet::wire::Encode(latest).size() <= 1200);
    assert(receiver.Accept(latest.commands.back().sequence));
    assert(!receiver.Accept(history.commands[1].sequence)); // Expired jump cannot replay again.
    sender.Acknowledge(receiver.sequence, receiver.bits);
    // Old retries and duplicated acknowledgements cannot discard newer commands.
    sender.Acknowledge(3, 7);
    assert(!sender.Packet().commands.empty());
    std::cout << "Input loss/reorder/duplicate/jump/window tests passed\n";
}

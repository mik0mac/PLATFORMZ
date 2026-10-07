// messages_test - MessageQueue::limit, the kill-feed cap (#178).
//
// The rule: duplicates always fold into the newest copy (which counts them);
// then, if still over MSG_MAX_ON_SCREEN lines, the oldest are dropped.
//
//   g++ -std=c++17 -I../server -I/opt/homebrew/include test/messages_test.cpp
//
// -I../server is the headless raylib stub.
#include "raylib.h"
#include "../messages.h"

#include <cstdio>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string& what) {
    printf("  %s %s\n", ok ? "ok:  " : "FAIL:", what.c_str());
    if (!ok) failures++;
}

static Message line(const std::string& text, float timeLeft = DEFAULT_MSG_DURATION) {
    Message m(MSG_TYPE_EXPLOSION_HIT, "", "");
    m.text = text;
    m.timeRemaining = timeLeft;
    return m;
}

// The feed as one string, oldest first, "TEXT*count" for folded lines.
static std::string feed(MessageQueue& q) {
    std::string s;
    for (const Message& m : q.getMessages()) {
        if (!s.empty()) s += " | ";
        s += m.text;
        if (m.count > 1) s += "*" + std::to_string(m.count);
    }
    return s;
}

static void expect(MessageQueue& q, const std::string& want, const char* what) {
    std::string got = feed(q);
    check(got == want, std::string(what) + "  ->  " + want);
    if (got != want) printf("        got: %s\n", got.c_str());
}

int main() {
    printf("cap is %d\n", MSG_MAX_ON_SCREEN);

    {   // Under the cap: duplicates still fold.
        MessageQueue q;
        q.push(line("A")); q.push(line("B")); q.push(line("A"));
        q.limit(4);
        expect(q, "B | A*2", "under the cap duplicates still fold");
    }
    {   // Under the cap with nothing repeated: untouched.
        MessageQueue q;
        q.push(line("A")); q.push(line("B")); q.push(line("C"));
        q.limit(4);
        expect(q, "A | B | C", "under the cap, no repeats -> untouched");
    }
    {   // Over the cap: duplicates fold into the newest copy, in its slot.
        MessageQueue q;
        q.push(line("HIT", 1.0f)); q.push(line("B")); q.push(line("HIT", 3.0f));
        q.push(line("C")); q.push(line("HIT", 5.0f));
        q.limit(4);
        expect(q, "B | C | HIT*3", "three hits fold into the newest");
        check(q.getMessages().back().timeRemaining == 5.0f, "folded line keeps the newest timer");
    }
    {   // Folding alone is enough: nobody is dropped.
        MessageQueue q;
        q.push(line("A")); q.push(line("B")); q.push(line("A"));
        q.push(line("C")); q.push(line("D"));
        q.limit(4);
        expect(q, "B | A*2 | C | D", "fold brings it to the cap, nothing dropped");
    }
    {   // No duplicates: the oldest go.
        MessageQueue q;
        for (const char* t : {"A", "B", "C", "D", "E", "F"}) q.push(line(t));
        q.limit(4);
        expect(q, "C | D | E | F", "no duplicates -> oldest two dropped");
    }
    {   // Both: fold first, then drop what is still over.
        MessageQueue q;
        for (const char* t : {"A", "X", "B", "X", "C", "D", "X"}) q.push(line(t));
        q.limit(4);
        expect(q, "B | C | D | X*3", "fold then drop the oldest");
    }
    {   // A folded line keeps folding on later frames.
        MessageQueue q;
        for (const char* t : {"X", "A", "B", "C", "X"}) q.push(line(t));
        q.limit(4);
        expect(q, "A | B | C | X*2", "first fold");
        q.push(line("X"));
        q.limit(4);
        expect(q, "A | B | C | X*3", "a later copy adds to the count");
    }
    {   // Same words, different colour: different message, not folded.
        MessageQueue q;
        Message red = line("A"); red.color = RED;
        q.push(red); q.push(line("A")); q.push(line("B")); q.push(line("C")); q.push(line("D"));
        q.limit(4);
        expect(q, "A | B | C | D", "different colours never fold; oldest dropped");
    }

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}

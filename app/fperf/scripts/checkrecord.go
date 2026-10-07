// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026, Krishnan Iyer

// Command checkrecord checks the answers fperf -r recorded.  For every request
// it works out what the unit in the request's slot must answer and compares
// that with the answer the FPGA gave, word by word.
//
//	go run app/fperf/scripts/checkrecord.go <dir>/record_thread_*.bin
//
// fperf -r (app/fperf/record.c) writes <-D dir>/record_thread_<id>.bin, all
// little endian:
//
//	file header  32 bytes: "FPERFREC", version, message size, seed, and the
//	             function of the unit in each of the 4 slots, a u16 each,
//	             0 for none
//	records      32 bytes: type (1 request, 2 response), sid, seq, time in
//	             ns, length, reserved; then that many bytes
//
// A request is the FRAC header and its data; its response carries the same
// sid and seq.  A request with no response is one the FPGA never answered.
//
// The models of top_k, log and norm are those of checkfuncs.go (scripts/ in
// the frac repository), which follow the RTL: keep the two in step.  The
// answers of cnn are not checked.
package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"sort"
	"strings"
)

const (
	// One 512-bit AXI-Stream beat.
	lineBytes    = 64
	wordsPerLine = lineBytes / 4
	slotCount    = 4

	recordMagic    = "FPERFREC"
	recordVersion  = 1
	recordRequest  = 1
	recordResponse = 2
)

// funcUnits names fperf's function ids (app/fperf/include/offrac.h).
var funcUnits = map[uint16]string{1: "top_k", 2: "cnn", 3: "log", 5: "norm"}

type fileHeader struct {
	Magic       [8]byte
	Version     uint32
	MessageSize uint32
	Seed        uint64
	SlotFunc    [slotCount]uint16
}

type recordHeader struct {
	Type     uint32
	Sid      uint32
	Seq      uint64
	TimeNs   uint64
	Len      uint32
	Reserved uint32
}

// ---------------------------------------------------------------- answers
// (checkfuncs.go's)

func f32(w uint32) float32 { return math.Float32frombits(w) }

// ftz flushes a subnormal to the zero of its sign, as the Xilinx cores do
// with their operands and results.
func ftz(f float32) float32 {
	if b := math.Float32bits(f); b&0x7f800000 == 0 {
		return math.Float32frombits(b & 0x80000000)
	}
	return f
}

// topK is top_k_core.v: the 16 largest words, unsigned, largest first, word j
// kept only where bit j of the mask is set.
func topK(words []uint32, mask uint16) []uint32 {
	ranked := append([]uint32(nil), words...)
	sort.Slice(ranked, func(a, b int) bool { return ranked[a] > ranked[b] })
	out := make([]uint32, wordsPerLine)
	for j := 0; j < wordsPerLine && j < len(ranked); j++ {
		if mask>>uint(j)&1 != 0 {
			out[j] = ranked[j]
		}
	}
	return out
}

// logit is log_core.v: 1 - x, then x / (1 - x), then its logarithm, each
// rounded to single precision.
func logit(words []uint32) []uint32 {
	out := make([]uint32, len(words))
	for idx, w := range words {
		x := ftz(f32(w))
		d := ftz(float32(1 - x))
		q := ftz(float32(x / d))
		out[idx] = math.Float32bits(ftz(float32(math.Log(float64(q)))))
	}
	return out
}

// fkey is norm_core.v's ordering: an unsigned compare of the keys orders the
// floats, -0 below +0.
func fkey(w uint32) uint32 {
	if w&0x80000000 != 0 {
		return ^w
	}
	return w | 0x80000000
}

// norm is norm_core.v: the min and max of the request, max - min once, then
// (x - min) / (max - min) for every value, each step rounded to single
// precision.
func norm(words []uint32) []uint32 {
	lo, hi := words[0], words[0]
	for _, w := range words[1:] {
		if fkey(w) < fkey(lo) {
			lo = w
		}
		if fkey(w) > fkey(hi) {
			hi = w
		}
	}
	low := ftz(f32(lo))
	span := ftz(float32(ftz(f32(hi)) - low))
	out := make([]uint32, len(words))
	for idx, w := range words {
		diff := ftz(float32(ftz(f32(w)) - low))
		out[idx] = math.Float32bits(ftz(float32(diff / span)))
	}
	return out
}

func wordsOf(data []byte) []uint32 {
	out := make([]uint32, len(data)/4)
	for idx := range out {
		out[idx] = binary.LittleEndian.Uint32(data[idx*4:])
	}
	return out
}

func bytesOf(values []uint32) []byte {
	out := make([]byte, len(values)*4)
	for idx, value := range values {
		binary.LittleEndian.PutUint32(out[idx*4:], value)
	}
	return out
}

func hexWords(data []byte) string {
	parts := make([]string, 0, len(data)/4)
	for _, w := range wordsOf(data) {
		parts = append(parts, fmt.Sprintf("%08x", w))
	}
	return strings.Join(parts, " ")
}

// ---------------------------------------------------------------- units

type unit struct {
	name string
	// answer is what the slot must send back for a request's data words and
	// the top config in its header, nil for a unit that is not checked.
	answer func(words []uint32, topConfig uint16) []uint32
	// A float unit is compared value by value, within ulps units in the
	// last place, or within nearZero of it; the others word for word.
	float    bool
	ulps     int64
	nearZero float64
}

func makeUnits(logULPs, normULPs int64) map[string]*unit {
	return map[string]*unit{
		"top_k": {name: "top_k", answer: topK},
		"log": {
			name:     "log",
			answer:   func(words []uint32, _ uint16) []uint32 { return logit(words) },
			float:    true,
			ulps:     logULPs,
			nearZero: 0x1p-24,
		},
		"norm": {
			name:   "norm",
			answer: func(words []uint32, _ uint16) []uint32 { return norm(words) },
			float:  true,
			ulps:   normULPs,
		},
		"cnn": {name: "cnn"},
	}
}

// ---------------------------------------------------------------- compare
// (checkfuncs.go's)

func isNaN(w uint32) bool { return w&0x7f800000 == 0x7f800000 && w&0x007fffff != 0 }
func isInf(w uint32) bool { return w&0x7fffffff == 0x7f800000 }

// ordinal numbers the floats in order, both zeros 0, so that two are as many
// units in the last place apart as their ordinals.
func ordinal(w uint32) int64 {
	if w&0x80000000 != 0 {
		return -int64(w & 0x7fffffff)
	}
	return int64(w)
}

type verdict struct {
	words, exact, wrong int
	maxULP              int64 // over the finite values, the accepted ones too
	notes               []string
}

// compare checks an answer of the right length against the expected one; in
// is the request's data, for the notes.
func compare(u *unit, in []uint32, want, got []byte, maxNotes int) verdict {
	var v verdict
	wantWords, gotWords := wordsOf(want), wordsOf(got)
	for idx, w := range wantWords {
		g := gotWords[idx]
		v.words++
		if w == g || (u.float && isNaN(w) && isNaN(g)) {
			v.exact++
			continue
		}
		ok := false
		var ulps int64
		if u.float && !isNaN(w) && !isNaN(g) && !isInf(w) && !isInf(g) {
			ulps = ordinal(w) - ordinal(g)
			if ulps < 0 {
				ulps = -ulps
			}
			if ulps > v.maxULP {
				v.maxULP = ulps
			}
			ok = ulps <= u.ulps ||
				(u.nearZero > 0 && math.Abs(float64(f32(w))-float64(f32(g))) <= u.nearZero)
		}
		if ok {
			continue
		}
		v.wrong++
		if len(v.notes) >= maxNotes {
			continue
		}
		where := fmt.Sprintf("line %d word %d", idx/wordsPerLine, idx%wordsPerLine)
		if !u.float {
			v.notes = append(v.notes, fmt.Sprintf("%s: want %08x, got %08x", where, w, g))
			continue
		}
		note := fmt.Sprintf("%s: x %v (%08x), want %v (%08x), got %v (%08x)",
			where, f32(in[idx]), in[idx], f32(w), w, f32(g), g)
		if ulps > 0 {
			note += fmt.Sprintf(", %d ulp", ulps)
		}
		v.notes = append(v.notes, note)
	}
	return v
}

// ---------------------------------------------------------------- records

type key struct {
	file int
	sid  uint32
	seq  uint64
}

type request struct {
	where string // file, sid and seq, for the report
	data  []byte
}

type slotReport struct {
	unit         string
	checked      int
	right        int
	skipped      int
	words, exact int
	maxULP       int64
	failures     []string // the first -show wrong answers
}

type checker struct {
	units    map[string]*unit
	show     int
	files    int
	slotUnit [slotCount]string // from the first file; every file must agree
	slots    [slotCount]*slotReport
	pending  map[key]request
	problems []string // records that make no sense
}

func (c *checker) problem(format string, args ...interface{}) {
	c.problems = append(c.problems, fmt.Sprintf(format, args...))
}

func (c *checker) readFile(idx int, path string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	r := bufio.NewReaderSize(f, 1<<20)

	var fh fileHeader
	if err := binary.Read(r, binary.LittleEndian, &fh); err != nil {
		return fmt.Errorf("reading the file header: %w", err)
	}
	if string(fh.Magic[:]) != recordMagic {
		return fmt.Errorf("not an fperf -r record")
	}
	if fh.Version != recordVersion {
		return fmt.Errorf("record version %d, this reads version %d", fh.Version, recordVersion)
	}
	var units [slotCount]string
	for slot, fn := range fh.SlotFunc {
		if fn == 0 {
			continue
		}
		if units[slot] = funcUnits[fn]; units[slot] == "" {
			return fmt.Errorf("slot %d holds function %d, which this does not know", slot, fn)
		}
	}
	if c.files == 0 {
		c.slotUnit = units
		for slot := range c.slots {
			c.slots[slot] = &slotReport{unit: units[slot]}
		}
	} else if units != c.slotUnit {
		return fmt.Errorf("its slots hold %v, the first file's %v: not the same run", units, c.slotUnit)
	}
	c.files++

	for {
		var h recordHeader
		if err := binary.Read(r, binary.LittleEndian, &h); err != nil {
			if errors.Is(err, io.EOF) {
				return nil
			}
			if errors.Is(err, io.ErrUnexpectedEOF) {
				c.problem("%s: ends in part of a record header", path)
				return nil
			}
			return err
		}
		data := make([]byte, h.Len)
		if _, err := io.ReadFull(r, data); err != nil {
			c.problem("%s: ends in part of the record of sid %d seq %d", path, h.Sid, h.Seq)
			return nil
		}

		k := key{idx, h.Sid, h.Seq}
		where := fmt.Sprintf("%s sid %d seq %d", path, h.Sid, h.Seq)
		switch h.Type {
		case recordRequest:
			if _, ok := c.pending[k]; ok {
				c.problem("%s: a second request before the answer to the first", where)
			}
			c.pending[k] = request{where, data}
		case recordResponse:
			req, ok := c.pending[k]
			if !ok {
				c.problem("%s: an answer to no request", where)
				continue
			}
			delete(c.pending, k)
			c.check(req, data)
		default:
			return fmt.Errorf("a record of type %d after sid %d seq %d", h.Type, h.Sid, h.Seq)
		}
	}
}

// check works out the answer to req and compares it with resp.
func (c *checker) check(req request, resp []byte) {
	if len(req.data) < 2*lineBytes || len(req.data)%lineBytes != 0 {
		c.problem("%s: a %d-byte request is not a header and whole data lines", req.where, len(req.data))
		return
	}
	header := req.data[:lineBytes]
	size := binary.LittleEndian.Uint32(header[56:60])
	topConfig := binary.LittleEndian.Uint16(header[60:62])
	slot := int(binary.LittleEndian.Uint16(header[62:64]))
	if int(size) != len(req.data) {
		c.problem("%s: the header says %d bytes, the request is %d", req.where, size, len(req.data))
		return
	}
	if slot >= slotCount {
		c.problem("%s: slot %d is not one of 0..%d", req.where, slot, slotCount-1)
		return
	}
	s := c.slots[slot]
	u := c.units[s.unit]
	if u == nil {
		c.problem("%s: sent to slot %d, which holds no unit", req.where, slot)
		return
	}
	if u.answer == nil {
		s.skipped++
		return
	}

	s.checked++
	in := wordsOf(req.data[lineBytes:])
	want := bytesOf(u.answer(in, topConfig))
	if len(resp) != len(want) {
		s.fail(c.show, fmt.Sprintf("  WRONG  %s, %d-byte request: %d answer bytes, want %d\n",
			req.where, size, len(resp), len(want)))
		return
	}
	v := compare(u, in, want, resp, 4)
	s.words += v.words
	s.exact += v.exact
	if v.maxULP > s.maxULP {
		s.maxULP = v.maxULP
	}
	if v.wrong == 0 {
		s.right++
		return
	}

	var b strings.Builder
	fmt.Fprintf(&b, "  WRONG  %s, %d-byte request", req.where, size)
	if u.name == "top_k" {
		fmt.Fprintf(&b, ", top config 0x%04x", topConfig)
	}
	fmt.Fprintf(&b, ": %d of %d words wrong\n", v.wrong, v.words)
	if u.name == "top_k" {
		fmt.Fprintf(&b, "    want %s\n    got  %s\n", hexWords(want), hexWords(resp))
	} else {
		for _, note := range v.notes {
			fmt.Fprintf(&b, "    %s\n", note)
		}
	}
	s.fail(c.show, b.String())
}

func (s *slotReport) fail(show int, text string) {
	if len(s.failures) < show {
		s.failures = append(s.failures, text)
	}
}

// report prints what was found and says whether all of it was right.
func (c *checker) report() bool {
	good := true
	var names []string
	for slot, name := range c.slotUnit {
		if name != "" {
			names = append(names, fmt.Sprintf("%d %s", slot, name))
		}
	}
	fmt.Printf("checkrecord: %d file(s), slots %s\n\n", c.files, strings.Join(names, ", "))

	for slot, s := range c.slots {
		if s.unit == "" {
			continue
		}
		fmt.Printf("slot %d %s: ", slot, s.unit)
		switch {
		case s.checked == 0 && s.skipped == 0:
			fmt.Println("no requests")
			continue
		case s.checked == 0:
			fmt.Printf("%d answers not checked\n", s.skipped)
			continue
		}
		status := "ok"
		if s.right != s.checked {
			status = "FAIL"
			good = false
		}
		fmt.Printf("%s, %d of %d answers right", status, s.right, s.checked)
		if u := c.units[s.unit]; u.float && s.words > 0 {
			fmt.Printf("; %d values, %.2f%% bit for bit, at most %d ulp off",
				s.words, 100*float64(s.exact)/float64(s.words), s.maxULP)
		}
		fmt.Println()
		for _, f := range s.failures {
			fmt.Print(f)
		}
	}

	if len(c.pending) > 0 {
		good = false
		var unanswered []string
		for _, req := range c.pending {
			text := fmt.Sprintf("  %s: %d bytes", req.where, len(req.data))
			if len(req.data) >= lineBytes {
				text += fmt.Sprintf(" to slot %d", binary.LittleEndian.Uint16(req.data[62:64]))
			}
			unanswered = append(unanswered, text)
		}
		sort.Strings(unanswered)
		fmt.Printf("\nunanswered: %d request(s)\n%s\n", len(unanswered), strings.Join(unanswered, "\n"))
	}
	if len(c.problems) > 0 {
		good = false
		fmt.Printf("\nproblems:\n  %s\n", strings.Join(c.problems, "\n  "))
	}

	fmt.Println()
	if !good {
		fmt.Println("FAIL")
		return false
	}
	fmt.Println("ok: every answer checked was right")
	return true
}

func main() {
	logULPs := flag.Int64("log-ulps", 2, "units in the last place log's answers may be off")
	normULPs := flag.Int64("norm-ulps", 0, "units in the last place norm's answers may be off")
	show := flag.Int("show", 3, "wrong answers to print per slot")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "usage: checkrecord [flags] record_thread_N.bin...\n")
		flag.PrintDefaults()
	}
	flag.Parse()
	if flag.NArg() == 0 {
		flag.Usage()
		os.Exit(2)
	}
	if *logULPs < 0 || *normULPs < 0 || *show < 0 {
		fmt.Fprintln(os.Stderr, "-log-ulps, -norm-ulps and -show must not be negative")
		os.Exit(2)
	}

	c := &checker{
		units:   makeUnits(*logULPs, *normULPs),
		show:    *show,
		pending: map[key]request{},
	}
	for idx, path := range flag.Args() {
		if err := c.readFile(idx, path); err != nil {
			fmt.Fprintf(os.Stderr, "%s: %v\n", path, err)
			os.Exit(1)
		}
	}
	if !c.report() {
		os.Exit(1)
	}
}

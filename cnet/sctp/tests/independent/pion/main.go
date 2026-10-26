// Test-only independent SCTP RFC8260 negotiation oracle.
// Pion is an external Go implementation, NOT vendored or linked into Salts.
// UDP carries raw SCTP packets exclusively inside this isolated CI fixture:
// it is NOT DTLS, a production transport, or browser/libwebrtc proof.
package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/pion/sctp"
)

func require(ok bool, format string, a ...interface{}) {
	if !ok {
		fmt.Fprintf(os.Stderr, "PION_ORACLE_FAILURE: "+format+"\n", a...)
		os.Exit(1)
	}
}

func expect(lines <-chan string, prefix string, timeout time.Duration) string {
	limit := time.After(timeout)
	for {
		select {
		case line, open := <-lines:
			require(open, "C11 oracle exited before %s", prefix)
			fmt.Println("C11:", line)
			if strings.HasPrefix(line, prefix) {
				return line
			}
		case <-limit:
			require(false, "timeout waiting for %s", prefix)
		}
	}
}

func main() {
	require(len(os.Args) == 2, "expected C11 oracle binary argument")
	peer := exec.Command(os.Args[1], "49571", "49572")
	out, err := peer.StdoutPipe()
	require(err == nil, "StdoutPipe: %v", err)
	peer.Stderr = os.Stderr
	require(peer.Start() == nil, "failed to launch %s", os.Args[1])
	defer func() {
		_ = peer.Process.Signal(syscall.SIGTERM)
		_ = peer.Wait()
	}()
	lines := make(chan string, 128)
	go func() {
		scanner := bufio.NewScanner(out)
		for scanner.Scan() {
			lines <- scanner.Text()
		}
		close(lines)
	}()
	expect(lines, "READY", 5*time.Second)

	local := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 49572}
	remote := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 49571}
	conn, err := net.DialUDP("udp", local, remote)
	require(err == nil, "UDP dial: %v", err)
	defer conn.Close()
	timer := time.AfterFunc(12*time.Second, func() { _ = conn.Close() })
	defer timer.Stop()
	association, err := sctp.ClientWithOptions(
		sctp.Config{NetConn: conn},
		sctp.WithEnableInterleaving(true),
	)
	require(err == nil, "independent Pion SCTP handshake: %v", err)
	defer association.Close()
	metadata, ok := association.Metadata()
	require(ok, "independent Pion SCTP metadata unavailable")
	fmt.Printf("PION_NEGOTIATION: interleaving=%t partial-reliability=%d\n",
		metadata.MessageInterleavingEnabled, metadata.PartialReliabilityMode)
	require(metadata.MessageInterleavingEnabled,
		"RFC8260 I-DATA was NOT independently negotiated")
	require(metadata.PartialReliabilityMode ==
		sctp.PartialReliabilityModeIForwardTSN,
		"expected independently negotiated RFC8260 I-FORWARD-TSN, got %d",
		metadata.PartialReliabilityMode)

	stream, err := association.OpenStream(0, sctp.PayloadTypeWebRTCDCEP)
	require(err == nil, "Pion DCEP OpenStream: %v", err)
	// RFC8832 DCEP OPEN, TIMED+UNORDERED 0x82, lifetime 1500ms.
	// DCEP PPID50 remains reliable and is never deliberately dropped.
	open := []byte{0x03, 0x82, 0, 0, 0, 0, 0x05, 0xdc, 0, 0, 0, 0}
	n, err := stream.WriteSCTP(open, sctp.PayloadTypeWebRTCDCEP)
	require(err == nil && n == len(open), "DCEP OPEN send: n=%d err=%v", n, err)
	_ = stream.SetReadDeadline(time.Now().Add(10 * time.Second))
	p := make([]byte, 128)
	n, ppi, err := stream.ReadSCTP(p)
	require(err == nil && n == 1 && p[0] == 2 &&
		ppi == sctp.PayloadTypeWebRTCDCEP,
		"expected C11 DCEP ACK, got n=%d ppi=%d err=%v", n, ppi, err)
	fmt.Println("PION_DCEP_ACK_OK")
	// The first C11 DATA must be decoded by the independent Go peer.
	n, ppi, err = stream.ReadSCTP(p)
	require(err == nil && ppi == sctp.PayloadTypeWebRTCBinary &&
		string(p[:n]) == "c11-nominal",
		"independent Pion did not receive nominal: n=%d ppi=%d err=%v value=%q",
		n, ppi, err, p[:n])
	fmt.Println("PION_RECEIVED_C11_NOMINAL_IDATA")
	// The C11 sender committed one subsequent DATA TSN but our test-only
	// datagram transport dropped it. Pion must process the real RFC8260
	// I-FORWARD-TSN and deliver ONLY the distinct following message.
	n, ppi, err = stream.ReadSCTP(p)
	require(err == nil && ppi == sctp.PayloadTypeWebRTCBinary &&
		string(p[:n]) == "c11-after-forward",
		"independent Pion failed I-FORWARD recovery: n=%d ppi=%d err=%v value=%q",
		n, ppi, err, p[:n])
	fmt.Println("PION_RECEIVED_POST_I_FORWARD_SUCCESSOR")
	stream.SetReliabilityParams(true, sctp.ReliabilityTypeTimed, 1500)
	payload := []byte("pion-independent-idata")
	n, err = stream.WriteSCTP(payload, sctp.PayloadTypeWebRTCBinary)
	require(err == nil && n == len(payload), "Pion I-DATA send: %v", err)
	// These two externally generated records are required: I-DATA=64 and
	// an Owner-delivered exact PPID53 message, NOT a self-loopback fixture.
	expected := "MESSAGE=0:" + hex.EncodeToString(payload)
	seenIData, seenMessage, seenForward, seenDrop, seenConfirmed := false, false, false, false, false
	limit := time.After(6 * time.Second)
	for !seenIData || !seenMessage || !seenForward || !seenDrop || !seenConfirmed {
		select {
		case line, open := <-lines:
			require(open, "C11 peer terminated during I-DATA")
			fmt.Println("C11:", line)
			if line == "RX_TYPE=64" { seenIData = true }
			if line == expected { seenMessage = true }
			if line == "C11_REAL_I_FORWARD_TSN_194" { seenForward = true }
			if line == "C11_DROPPED_ONE_I_DATA" { seenDrop = true }
			if line == "C11_PEER_CONFIRMED_I_FORWARD" { seenConfirmed = true }
		case <-limit:
			require(false,
				"RFC8260 evidence missing: type64=%t message=%t forward=%t drop=%t peerACK=%t",
				seenIData, seenMessage, seenForward, seenDrop, seenConfirmed)
		}
	}
	// Real SCTP RFC8260 I-DATA and I-FORWARD peer recovery over *test-only*
	// UDP datagrams. This is NOT authenticated DTLS/ICE/browser evidence.
	fmt.Println("PION_INDEPENDENT_RFC8260_IFORWARD_PASS " +
		strconv.Itoa(int(metadata.PartialReliabilityMode)))
}

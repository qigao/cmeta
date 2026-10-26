// Test-only independent SCTP RFC8260 negotiation oracle.
// Pion is an external Go implementation, NOT vendored or linked into Salts.
// UDP carries raw SCTP packets exclusively inside this isolated CI fixture:
// it is NOT DTLS, a production transport, or browser/libwebrtc proof.
package main

import (
	"bufio"
	"context"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"syscall"
	"time"

	dtls "github.com/pion/dtls/v3"
	"github.com/pion/dtls/v3/pkg/crypto/selfsign"
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
	require(len(os.Args) >= 2 && len(os.Args) <= 4,
		"expected C11 oracle binary [drop-first-iforward] [dtls|dtls-bad-server-fingerprint|dtls-bad-client-fingerprint]")
	dropFirstForward, useDTLS, badServerFingerprint, badClientFingerprint := false, false, false, false
	for _, option := range os.Args[2:] {
		switch option {
		case "drop-first-iforward":
			require(!dropFirstForward, "duplicate I-FORWARD drop option")
			dropFirstForward = true
		case "dtls":
			require(!useDTLS, "duplicate DTLS option")
			useDTLS = true
		case "dtls-bad-server-fingerprint":
			require(!useDTLS, "duplicate DTLS option")
			useDTLS, badServerFingerprint = true, true
		case "dtls-bad-client-fingerprint":
			require(!useDTLS, "duplicate DTLS option")
			useDTLS, badClientFingerprint = true, true
		default:
			require(false, "unknown mode %s", option)
		}
	}
	require((!badServerFingerprint && !badClientFingerprint) ||
			!dropFirstForward,
		"wrong-fingerprint test must not run SCTP loss scenarios")
	peerArgs := []string{"49571", "49572"}
	var certificate tls.Certificate
	if useDTLS {
		var certErr error
		certificate, certErr = selfsign.GenerateSelfSigned()
		require(certErr == nil && len(certificate.Certificate) != 0,
			"ephemeral Pion DTLS certificate: %v", certErr)
		clientFingerprint := sha256.Sum256(certificate.Certificate[0])
		if badClientFingerprint {
			/* Simulate a corrupted trusted remote SDP client fingerprint.
			 * The actual Pion client certificate remains unchanged. */
			clientFingerprint[0] ^= 1
		}
		peerArgs = append(peerArgs, "dtls",
			hex.EncodeToString(clientFingerprint[:]))
		if dropFirstForward {
			peerArgs = append(peerArgs, "drop-first-iforward")
		}
	} else if dropFirstForward {
		peerArgs = append(peerArgs, "drop-first-iforward")
	}
	peer := exec.Command(os.Args[1], peerArgs...)
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
	var expectedServerFingerprint [32]byte
	if useDTLS {
		line := expect(lines, "DTLS_SERVER_FP=", 5*time.Second)
		hexString := strings.TrimPrefix(line, "DTLS_SERVER_FP=")
		der, decodeErr := hex.DecodeString(hexString)
		require(decodeErr == nil && len(der) == 32,
			"OpenSSL DTLS server did not publish valid pinned fingerprint")
		copy(expectedServerFingerprint[:], der)
		if badServerFingerprint {
			/* Simulate a corrupted SDP fingerprint from the trusted offer.
			 * The DTLS certificate stays genuine; a successful handshake
			 * would therefore be a credential validation vulnerability. */
			expectedServerFingerprint[0] ^= 1
		}
	}
	expect(lines, "READY", 5*time.Second)

	local := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 49572}
	remote := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 49571}
	var conn net.Conn
	// err is already scoped to the earlier StdoutPipe/peer launch.
	if useDTLS {
		raw, listenErr := net.ListenUDP("udp", local)
		require(listenErr == nil, "DTLS socket listen: %v", listenErr)
		pinned, rejectedBadServerPin := false, false
		secure, dtlsErr := dtls.Client(raw, remote, &dtls.Config{
			Certificates:       []tls.Certificate{certificate},
			InsecureSkipVerify: true, // bounded to the SHA256 check BELOW
			VerifyPeerCertificate: func(rawCerts [][]byte,
				_ [][]*x509.Certificate) error {
				if len(rawCerts) == 0 {
					return errors.New("OpenSSL DTLS certificate absent")
				}
				actual := sha256.Sum256(rawCerts[0])
				if actual != expectedServerFingerprint {
					rejectedBadServerPin = true
					return errors.New("OpenSSL DTLS SHA256 fingerprint mismatch")
				}
				pinned = true
				return nil
			},
		})
		require(dtlsErr == nil, "independent Pion DTLS client: %v", dtlsErr)
		ctx, cancel := context.WithTimeout(context.Background(), 9*time.Second)
		err = secure.HandshakeContext(ctx)
		cancel()
		if badServerFingerprint {
			require(err != nil && rejectedBadServerPin && !pinned,
				"wrong OpenSSL SHA256 pin was not explicitly rejected: err=%v callback=%t",
				err, rejectedBadServerPin)
			_ = secure.Close()
			fmt.Println("PION_DTLS_WRONG_SERVER_FINGERPRINT_REJECTED")
			return // no SCTP association, INIT, DCEP or payload admitted
		}
		if badClientFingerprint {
			/* The remote OpenSSL endpoint checks the genuine Pion leaf
			 * against a corrupted expected SHA256 pin. Its refusal may
			 * become visible asynchronously to the DTLS client, so wait
			 * for the EXACT native certificate-rejection marker. */
			expect(lines, "OPENSSL_DTLS_BAD_CLIENT_FINGERPRINT_REJECTED",
				6*time.Second)
			_ = secure.Close()
			fmt.Println("PION_DTLS_WRONG_CLIENT_FINGERPRINT_REJECTED")
			return // never create SCTP on an invalid authenticated peer
		}
		require(err == nil && pinned,
			"independent Pion/OpenSSL DTLS 1.2 handshake or pin: %v pinned=%t",
			err, pinned)
		conn = secure
		fmt.Println("PION_DTLS_1_2_PINNED_OPENSSL_SERVER_PASS")
	} else {
		conn, err = net.DialUDP("udp", local, remote)
		require(err == nil, "UDP dial: %v", err)
	}
	defer conn.Close()
	timer := time.AfterFunc(13*time.Second, func() { _ = conn.Close() })
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
	// The RFC8832 ACK is ordered/reliable PPID50, while the newly opened
	// application's I-DATA is TIMED+UNORDERED PPID53. Independent SCTP
	// implementations may surface those separate message spaces in either
	// order. Require BOTH exactly once, never assume ACK comes first.
	seenAck, seenNominal := false, false
	for i := 0; i < 2; i++ {
		n0, ppi0, readErr := stream.ReadSCTP(p)
		require(readErr == nil, "independent Pion read ACK/nominal: %v", readErr)
		switch {
		case ppi0 == sctp.PayloadTypeWebRTCDCEP:
			require(!seenAck && n0 == 1 && p[0] == 2,
				"duplicate/bad DCEP ACK n=%d", n0)
			seenAck = true
		case ppi0 == sctp.PayloadTypeWebRTCBinary:
			require(!seenNominal && string(p[:n0]) == "c11-nominal",
				"unexpected pre-FORWARD data %q", p[:n0])
			seenNominal = true
		default:
			require(false, "unexpected PPID %d during DCEP+I-DATA", ppi0)
		}
	}
	require(seenAck && seenNominal, "DCEP ACK or nominal I-DATA missing")
	fmt.Println("PION_DCEP_ACK_OK")
	fmt.Println("PION_RECEIVED_C11_NOMINAL_IDATA")
	// The next C11 DATA must be released only by real I-FORWARD-TSN.
	// n was already declared by the original DCEP send in this scope.
	var ppi sctp.PayloadProtocolIdentifier
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
	seenForwardLoss, seenForwardRetry, seenPinnedClient := false, false, false
	limit := time.After(6 * time.Second)
	for !seenIData || !seenMessage || !seenForward || !seenDrop ||
		!seenConfirmed || (dropFirstForward && (!seenForwardLoss || !seenForwardRetry)) ||
		(useDTLS && !seenPinnedClient) {
		select {
		case line, open := <-lines:
			require(open, "C11 peer terminated during I-DATA")
			fmt.Println("C11:", line)
			if line == "OPENSSL_DTLS_CLIENT_FINGERPRINT_MATCH" {
				require(useDTLS, "unexpected DTLS fingerprint event in plaintext mode")
				seenPinnedClient = true
			}
			if line == "RX_TYPE=64" { seenIData = true }
			if line == expected { seenMessage = true }
			if line == "C11_REAL_I_FORWARD_TSN_194" { seenForward = true }
			if line == "C11_DROPPED_ONE_I_DATA" { seenDrop = true }
			if line == "C11_PEER_CONFIRMED_I_FORWARD" { seenConfirmed = true }
			if line == "C11_DROPPED_FIRST_I_FORWARD_194" {
				require(dropFirstForward, "unexpected I-FORWARD loss in baseline mode")
				seenForwardLoss = true
			}
			if line == "C11_RETRANSMITTED_SAME_I_FORWARD_194" {
				require(dropFirstForward, "unexpected I-FORWARD replay in baseline mode")
				seenForwardRetry = true
			}
		case <-limit:
			require(false,
				"RFC8260 evidence missing: type64=%t message=%t forward=%t drop=%t peerACK=%t fwdLost=%t fwdRetried=%t",
				seenIData, seenMessage, seenForward, seenDrop, seenConfirmed,
				seenForwardLoss, seenForwardRetry)
		}
	}
	// Real SCTP RFC8260 I-DATA and I-FORWARD peer recovery over *test-only*
	// UDP datagrams. This is NOT authenticated DTLS/ICE/browser evidence.
	mode := "baseline"
	if dropFirstForward {
		mode = "lost-first-iforward"
	}
	transport := "udp-plaintext"
	if useDTLS {
		transport = "pion-dtls-v3-to-openssl-dtls12"
	}
	fmt.Println("PION_INDEPENDENT_RFC8260_IFORWARD_PASS transport=" + transport + " mode=" + mode +
		" partialReliability=" + strconv.Itoa(int(metadata.PartialReliabilityMode)))
}

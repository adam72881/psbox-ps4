, n, now);
            rtp.push_back(std::move(message));
        }
    }
    messages.swap(rtp);
    // Solo accumulo qui: TWCC/REMB partono da pump() sul thread app.
    // Mandare RTCP dal path RTP (con mu_ tenuto) su Orbis bloccava il recv
    // DTLS/ICE e contribuiva a feedback di congestione spurii.
}

}  // namespace xc::stream

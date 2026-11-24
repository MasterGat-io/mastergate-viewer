#!/bin/sh
if [[ $SKIP_NOTARIZATION == "true" ]]; then
    echo "Skipping notarization"
    exit 0
fi

CONFIG_FILE="$build_secrets_checkout/code-signing-osx/notarize_creds.sh"
if [[ -f "$CONFIG_FILE" ]]; then
    source "$CONFIG_FILE"
    app_file="$1"
    zip_file=${app_file/app/zip}
    ditto -c -k --keepParent "$app_file" "$zip_file"
    if [[ -f "$zip_file" ]]; then
        # res=$(xcrun notarytool store-credentials \
        #                         viewer.profile \
        #                         --verbose 2>*1)
        # echo $res
        # <MG> an App Store Connect API key (ASC_KEY_PATH / ASC_KEY_ID / ASC_ISSUER) or an Apple ID.
        # Submit without --wait, then poll: a GitHub runner's network can drop for a while
        # and "--wait" then fails although Apple is still working on the submission.
        if [[ -n "$ASC_KEY_ID" ]]; then
            auth=(--key "$ASC_KEY_PATH" --key-id "$ASC_KEY_ID" --issuer "$ASC_ISSUER")
        else
            auth=(--apple-id "$USERNAME" --password "$PASSWORD" --team-id "$ASC_PROVIDER")
        fi
        match=""
        for submit_try in 1 2 3; do
            res=$(xcrun notarytool submit "$zip_file" "${auth[@]}" --no-wait 2>&1)
            echo "Notarytool submit (try $submit_try):"
            echo "$res"
            if [[ "$res" =~ id:\ ([0-9a-fA-F-]{36}) ]]; then
                match="${BASH_REMATCH[1]}"
                break
            fi
            sleep 30
        done
        if [[ -z "$match" ]]; then
            echo "No submission id"
            exit 1
        fi
        echo "Submitted with id: [$match]"

        # up to 90 minutes (the first submissions of a new team can take a while)
        status=""
        for ((poll = 1; poll <= 90; poll++)); do
            info=$(xcrun notarytool info "$match" "${auth[@]}" 2>&1)
            if [[ "$info" =~ status:\ (Accepted|Invalid|Rejected|In\ Progress) ]]; then
                status="${BASH_REMATCH[1]}"
            else
                status="unknown (network?)"
            fi
            echo "Notarization status after $poll min: $status"
            if [[ "$status" == "Accepted" ]]; then
                break
            fi
            if [[ "$status" == "Invalid" || "$status" == "Rejected" ]]; then
                xcrun notarytool log "$match" "${auth[@]}"
                exit 1
            fi
            sleep 60
        done
        if [[ "$status" != "Accepted" ]]; then
            echo "Notarization not finished after 90 minutes (id $match)"
            exit 1
        fi
        echo "Notarized with id: [$match]"
        # </MG>

        # --- WAIT / RETRY LOGIC FOR STAPLER ---
        # Try stapler up to 5 times with a 5-second delay in between attempts
        max_attempts=5
        sleep_seconds=5
        attempt=1

        while [[ $attempt -le $max_attempts ]]; do
            echo "Running stapler (attempt $attempt of $max_attempts)..."
            xcrun stapler staple "$app_file"
            ret=$?

            if [[ $ret -eq 0 ]]; then
                echo "Stapling succeeded on attempt $attempt"
                break
            else
                if [[ $attempt -lt $max_attempts ]]; then
                    echo "Stapling failed (Error $ret). Waiting $sleep_seconds seconds before retry..."
                    sleep $sleep_seconds
                else
                    echo "Stapling failed after $max_attempts attempts, giving up."
                    exit 65
                fi
            fi
            attempt=$((attempt + 1))
        done

        # Delete the zip file to stop it being packed in the dmg
        rm -f "$zip_file"
        if [[ $? -eq 0 ]]; then
            echo "$zip_file deleted successfully."
        else
            echo "Failed to delete $zip_file"
        fi
        exit 0
        # else
            # echo "Notarization error"
            # exit 1
        # fi
    else
        echo "Notarization error: ditto failed"
        exit 1
    fi
else
    echo "No config file found - check notarize_creds is present in the secrets"
    exit 1
fi
